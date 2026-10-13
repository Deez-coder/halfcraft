package dev.halfcraft.passthrough.client;

import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.backend.opengl.GlTexture;
import dev.halfcraft.passthrough.Passthrough;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import org.lwjgl.opengl.EXTMemoryObject;
import org.lwjgl.opengl.EXTMemoryObjectWin32;
import org.lwjgl.opengl.EXTSemaphore;
import org.lwjgl.opengl.EXTSemaphoreWin32;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL45C;
import org.lwjgl.opengl.GLCapabilities;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.system.MemoryUtil;

/**
 * Zero copy: Minecraft's layers go straight into textures the host made on its own (D3D12) device and shares by name,
 * GPU to GPU, instead of ~25 MB a frame read back to memory, copied through the shared memory and uploaded again.
 *
 * <pre>
 * "Local\MCPassthroughGpu" (4096 bytes, the host writes it):
 *   0 int magic "MCGP"   4 int version (1)   8 int generation (new textures, new names)   12 int width   16 int height
 *   20 int slots (as FrameExporter's: a frame's textures are its slot's)   24 int host pid   28 int ready (1)
 *   32 long allocation size of each layer's textures (world, depth, overlay: 32, 40, 48)
 *   64 int the generation Minecraft took (written here)   68 int a generation Minecraft couldn't take (written here)
 * names: Local\MCPT_(pid)_(generation)_(slot)_(layer) for the textures, Local\MCPT_(pid)_(generation)_f for the fence
 * </pre>
 *
 * Layers: world colour and overlay RGBA8 (copied texture to texture), depth R32F (through a buffer in video memory:
 * a depth texture can't be copied into a colour one). Then the shared fence is signalled with the frame's number in it;
 * the host checks it has passed before it uses the slot. Anything missing (an older host, no GL support, a failed
 * import, a size change) and FrameExporter sends the frame the old way.
 */
final class GpuShare {
	private static final String NAME = "Local\\MCPassthroughGpu";
	private static final int MAGIC = 0x5047434D;
	private static final int LAYERS = 3;
	private static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT_UNALIGNED;
	private static final ValueLayout.OfLong LONG = ValueLayout.JAVA_LONG_UNALIGNED;
	private static final int[] FORMATS = {GL45C.GL_RGBA8, GL45C.GL_R32F, GL45C.GL_RGBA8};

	private static SharedMemory shm;
	/** No zero copy this session (no GL support, Minecraft's targets aren't plain RGBA8 GL textures...). */
	private static boolean unsupported;
	private static int generation;
	private static int failedGeneration;
	private static int width;
	private static int height;
	private static int[][] textures = new int[0][];
	private static int[][] memories = new int[0][];
	private static int semaphore;
	private static int depthBuffer;
	private static long fenceValue;

	private GpuShare() {
	}

	/** Render thread: whether this frame (its colour target, its size) goes straight into the host's textures. */
	static boolean ready(final GpuTexture color, final GpuTexture depth, final int w, final int h) {
		if (unsupported) {
			return false;
		}

		if (shm == null) {
			GLCapabilities caps = GL.getCapabilities();
			// (Minecraft asks for GL 3.3: what's used here comes as extensions, through GL45C's entry points)
			boolean gl = caps.GL_ARB_direct_state_access && caps.GL_ARB_copy_image && caps.GL_ARB_buffer_storage;
			if (!caps.GL_EXT_memory_object_win32 || !gl || !(color instanceof GlTexture c) || !(depth instanceof GlTexture)
				|| GL45C.glGetTextureLevelParameteri(c.glId(), 0, GL45C.GL_TEXTURE_INTERNAL_FORMAT) != GL45C.GL_RGBA8) {
				unsupported = true;
				Passthrough.LOG.info("frame export: no zero copy (memory objects {}, DSA/copy image/buffer storage {}, colour {})", caps.GL_EXT_memory_object_win32, gl,
					color instanceof GlTexture c ? Integer.toHexString(GL45C.glGetTextureLevelParameteri(c.glId(), 0, GL45C.GL_TEXTURE_INTERNAL_FORMAT)) : color);
				return false;
			}

			try {
				shm = SharedMemory.create(NAME, 4096L);
			} catch (Throwable t) {
				unsupported = true;
				Passthrough.LOG.warn("frame export: no zero copy: {}", t.toString());
				return false;
			}
		}

		MemorySegment m = shm.segment;
		if (m.get(INT, 0) != MAGIC || m.get(INT, 28) != 1) {
			return false;
		}

		int gen = m.get(INT, 8);
		if (gen != generation) {
			if (gen == failedGeneration) {
				return false;
			}

			release();
			if (!take(m, gen)) {
				release();
				failedGeneration = gen;
				m.set(INT, 68, gen);
				return false;
			}

			generation = gen;
			m.set(INT, 64, gen);
		}

		return width == w && height == h;
	}

	/** Open the host's textures and fence (generation gen) by name. */
	private static boolean take(final MemorySegment m, final int gen) {
		int w = m.get(INT, 12), h = m.get(INT, 16), slots = m.get(INT, 20), pid = m.get(INT, 24);
		if (slots != FrameExporter.SLOTS || w <= 0 || h <= 0) {
			return false;
		}

		while (GL45C.glGetError() != 0) {
			// (whatever was pending isn't ours)
		}

		textures = new int[slots][LAYERS];
		memories = new int[slots][LAYERS];
		String prefix = "Local\\MCPT_" + Integer.toUnsignedString(pid) + "_" + gen + "_";
		for (int s = 0; s < slots; s++) {
			for (int l = 0; l < LAYERS; l++) {
				int mem = EXTMemoryObject.glCreateMemoryObjectsEXT();
				memories[s][l] = mem;
				EXTMemoryObject.glMemoryObjectParameterivEXT(mem, EXTMemoryObject.GL_DEDICATED_MEMORY_OBJECT_EXT, new int[] {1});
				ByteBuffer name = MemoryUtil.memUTF16(prefix + s + "_" + l);
				EXTMemoryObjectWin32.glImportMemoryWin32NameEXT(mem, m.get(LONG, 32 + 8L * l), EXTMemoryObjectWin32.GL_HANDLE_TYPE_D3D12_RESOURCE_EXT,
					MemoryUtil.memAddress(name));
				MemoryUtil.memFree(name);
				int tex = GL45C.glCreateTextures(GL45C.GL_TEXTURE_2D);
				textures[s][l] = tex;
				EXTMemoryObject.glTextureStorageMem2DEXT(tex, 1, FORMATS[l], w, h, mem, 0L);
				int error = GL45C.glGetError();
				if (error != 0) {
					Passthrough.LOG.warn("frame export: no zero copy: GL error {} taking the host's texture {}_{}", Integer.toHexString(error), s, l);
					return false;
				}
			}
		}

		depthBuffer = GL45C.glCreateBuffers();
		GL45C.glNamedBufferStorage(depthBuffer, (long)w * h * 4, 0);
		// the fence: without it (no semaphore support) the host trusts that a published frame is done (it is: the
		// render thread waits for the GPU before it publishes)
		if (GL.getCapabilities().GL_EXT_semaphore_win32) {
			semaphore = EXTSemaphore.glGenSemaphoresEXT();
			ByteBuffer name = MemoryUtil.memUTF16(prefix + "f");
			EXTSemaphoreWin32.glImportSemaphoreWin32NameEXT(semaphore, EXTSemaphoreWin32.GL_HANDLE_TYPE_D3D12_FENCE_EXT, MemoryUtil.memAddress(name));
			MemoryUtil.memFree(name);
			int error = GL45C.glGetError();
			if (error != 0) {
				Passthrough.LOG.warn("frame export: the host's fence: GL error {}, going without", Integer.toHexString(error));
				EXTSemaphore.glDeleteSemaphoresEXT(semaphore);
				semaphore = 0;
			}
		}

		int error = GL45C.glGetError();
		if (error != 0) {
			Passthrough.LOG.warn("frame export: no zero copy: GL error {}", Integer.toHexString(error));
			return false;
		}

		width = w;
		height = h;
		fenceValue = 0L;
		Passthrough.LOG.info("frame export: zero copy on ({}x{}, generation {}, fence {})", w, h, gen, semaphore != 0 ? "yes" : "no");
		return true;
	}

	private static void release() {
		for (int s = 0; s < textures.length; s++) {
			for (int l = 0; l < LAYERS; l++) {
				if (textures[s][l] != 0) {
					GL45C.glDeleteTextures(textures[s][l]);
				}

				if (memories[s][l] != 0) {
					EXTMemoryObject.glDeleteMemoryObjectsEXT(memories[s][l]);
				}
			}
		}

		textures = new int[0][];
		memories = new int[0][];
		if (semaphore != 0) {
			EXTSemaphore.glDeleteSemaphoresEXT(semaphore);
			semaphore = 0;
		}

		if (depthBuffer != 0) {
			GL45C.glDeleteBuffers(depthBuffer);
			depthBuffer = 0;
		}

		generation = 0;
		width = height = 0;
	}

	/** Something here failed mid-frame: the old way from now on (the frame it happened in is lost). */
	static void fail(final RuntimeException e) {
		Passthrough.LOG.error("frame export: zero copy failed, frames go the old way from now on", e);
		unsupported = true;
		release();
	}

	/** The generation the frames written now belong to. */
	static int generation() {
		return generation;
	}

	/** The world layer (colour and depth) into slot's textures. */
	static void copyWorld(final GpuTexture color, final GpuTexture depth, final int slot) {
		copy(color, textures[slot][0]);
		// depth: read into the buffer, written from it into the R32F texture (GPU to GPU); Minecraft's pixel store and
		// buffer bindings are put back as they were
		int[] names = {GL45C.GL_PIXEL_PACK_BUFFER_BINDING, GL45C.GL_PIXEL_UNPACK_BUFFER_BINDING, GL45C.GL_PACK_ALIGNMENT, GL45C.GL_PACK_ROW_LENGTH,
			GL45C.GL_PACK_SKIP_ROWS, GL45C.GL_PACK_SKIP_PIXELS, GL45C.GL_UNPACK_ALIGNMENT, GL45C.GL_UNPACK_ROW_LENGTH, GL45C.GL_UNPACK_SKIP_ROWS,
			GL45C.GL_UNPACK_SKIP_PIXELS};
		int[] saved = new int[names.length];
		for (int i = 0; i < names.length; i++) {
			saved[i] = GL45C.glGetInteger(names[i]);
		}

		for (int i = 2; i < names.length; i++) {
			GL45C.glPixelStorei(names[i], names[i] == GL45C.GL_PACK_ALIGNMENT || names[i] == GL45C.GL_UNPACK_ALIGNMENT ? 4 : 0);
		}

		GL45C.glBindBuffer(GL45C.GL_PIXEL_PACK_BUFFER, depthBuffer);
		GL45C.glGetTextureImage(((GlTexture)depth).glId(), 0, GL45C.GL_DEPTH_COMPONENT, GL45C.GL_FLOAT, width * height * 4, 0L);
		GL45C.glBindBuffer(GL45C.GL_PIXEL_UNPACK_BUFFER, depthBuffer);
		GL45C.glTextureSubImage2D(textures[slot][1], 0, 0, 0, width, height, GL45C.GL_RED, GL45C.GL_FLOAT, 0L);
		GL45C.glBindBuffer(GL45C.GL_PIXEL_PACK_BUFFER, saved[0]);
		GL45C.glBindBuffer(GL45C.GL_PIXEL_UNPACK_BUFFER, saved[1]);
		for (int i = 2; i < names.length; i++) {
			GL45C.glPixelStorei(names[i], saved[i]);
		}
	}

	/** The overlay layer into slot's textures, then the fence: the value the host waits for (0: no fence). */
	static long copyOverlay(final GpuTexture color, final int slot) {
		copy(color, textures[slot][2]);
		if (semaphore == 0) {
			return 0L;
		}

		fenceValue++;
		try (MemoryStack stack = MemoryStack.stackPush()) {
			EXTSemaphore.glSemaphoreParameterui64vEXT(semaphore, EXTSemaphoreWin32.GL_D3D12_FENCE_VALUE_EXT, stack.longs(fenceValue));
			int layout = EXTSemaphore.GL_LAYOUT_SHADER_READ_ONLY_EXT;
			EXTSemaphore.glSignalSemaphoreEXT(semaphore, stack.mallocInt(0), stack.ints(textures[slot]), stack.ints(layout, layout, layout));
		}

		return fenceValue;
	}

	private static void copy(final GpuTexture from, final int to) {
		GL45C.glCopyImageSubData(((GlTexture)from).glId(), GL45C.GL_TEXTURE_2D, 0, 0, 0, 0, to, GL45C.GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);
	}
}
