package sun.java2d.opengl;

import sun.java2d.pipe.RenderBuffer;
import sun.java2d.pipe.RenderQueue;
import sun.misc.Unsafe;

import static sun.java2d.pipe.BufferedOpCodes.MASK_BUFFER_FENCE;
import static sun.java2d.pipe.BufferedOpCodes.TURBO_MASK_FILL;

public class OGLMaskBuffer {
  public static final int MASK_BUFFER_REGION_COUNT = 4;
  public static final int MASK_BUFFER_REGION_SIZE = 1024*1024;
  public static final int MASK_BUFFER_SIZE = MASK_BUFFER_REGION_SIZE * MASK_BUFFER_REGION_COUNT;

  /**
   * Vertex-Data per Mask-Quad: 1 Vertex = 8*sizeof(float) = 32 byte
   * Per Quad: 32x4 = 128 byte
   * -> Size Vertex Buffers, so that Masks with 512 byte on average fit the vtx buffer
   */
  public static final int VERTEX_BUFFER_SIZE = MASK_BUFFER_SIZE / 4;

  long maskBufferBasePtr;
  long vertexBufferBasePtr;

  long tileDataOffset = VERTEX_BUFFER_SIZE/2;

  int currentBufferOffset;

  int currentVtxPos;
  int lastVtxPos;

  private volatile static OGLMaskBuffer buffer;

  public static final Unsafe UNSAFE;

  private static int BUFFER_ARRAY_STRIDE = 1;

  private final static boolean pendingFences[] = new boolean[MASK_BUFFER_REGION_COUNT];

  static {
    UNSAFE = Unsafe.getUnsafe();
  }

  public static OGLMaskBuffer getInstance() {
    if(buffer == null) {
      synchronized(OGLMaskBuffer.class) {
        if(buffer == null) {
          OGLRenderQueue.getInstance().flushAndInvokeNow(new Runnable() {
            @Override
            public void run() {
              buffer = new OGLMaskBuffer();
            }
          });
        }
      }
    }

    return buffer;
  }

  public OGLMaskBuffer() {
    vertexBufferBasePtr = allocateVertexBufferPtr(VERTEX_BUFFER_SIZE);
    maskBufferBasePtr = allocateMaskBufferPtr(MASK_BUFFER_SIZE, MASK_BUFFER_REGION_SIZE);

    currentVtxPos = 0;
    lastVtxPos = 0;
    currentBufferOffset = 0;
  }

  public final int allocateMaskData(RenderQueue queue, int maskSize) {
    int offsetBefore = currentBufferOffset;

    // System.out.println("Requested " +maskSize +" offset before: " + offsetBefore);

    //int maskSize = w * h;

    int regionBefore = currentBufferOffset / MASK_BUFFER_REGION_SIZE;

    if (currentBufferOffset + maskSize >= MASK_BUFFER_SIZE) {
      offsetBefore = currentBufferOffset = 0;
    }

    currentBufferOffset += maskSize;

    // Align offset to multiple of 8
    currentBufferOffset = (currentBufferOffset + 7) & ~7;

    int regionAfter = currentBufferOffset / MASK_BUFFER_REGION_SIZE;

    if (regionBefore != regionAfter) {
      offsetBefore = (MASK_BUFFER_REGION_SIZE * regionAfter);
      currentBufferOffset = offsetBefore + maskSize;

      queue.ensureCapacity(12);
      RenderBuffer buffer = queue.getBuffer();
      buffer.putInt(MASK_BUFFER_FENCE);
      buffer.putInt(regionBefore);

      boolean nextRegionPending;
      synchronized (pendingFences) {
        // Optimization: Queue the wait for a fence one region before we actually need the region to be available.
        // Therefore we might save a few flushNow() synchronizations,
        // because the wait for region we need has been queued before (and maybe flushed because other draw calls)
        // E.g.: region 3 was just filled up -> place fence for region 3
        // and queue a wait for region region 1.
        // region 0's wait we require now was placed earlier, and might have been processed without an explicit flushNow
        int waitRegion = (regionBefore + 2) % MASK_BUFFER_REGION_COUNT;
        if (!pendingFences[waitRegion]) {
          waitRegion = -1;
        }
        buffer.putInt(waitRegion);
        //System.out.println("BUF: placing fence for:"+regionBefore+" waiting for: "+waitRegion);

        // enable in case async flush is available
        //queue.flushNow(false);

        pendingFences[regionBefore] = true;

        // We need the next region now, so check, if it is already available (fence was waited on earlier)
        nextRegionPending = pendingFences[regionAfter];
      }

      // If fence was not met, we have to explicitly flush, the wait has been queued earlier,
      // but was not yet processed by the OpenGL queue thread.
      int fenceCounter = 0;
      while (nextRegionPending) {
        //System.out.println("explicit flush");
        queue.flushNow();
        synchronized (pendingFences) {
          nextRegionPending = pendingFences[regionAfter];
        }

        // should not happen in sync flush mode
        if (fenceCounter > 0) {
          throw new RuntimeException("Aborting fence stall");
        }
        fenceCounter++;
      }
    }

    return offsetBefore;
  }

  public final long getMaskBufferBasePtr() {
    return maskBufferBasePtr;
  }

  private static void setFenceAvailable(int fenceNum) {
    synchronized (pendingFences) {
     // System.out.println("Fence available: " + fenceNum);
      pendingFences[fenceNum] = false;
    }
  }

  private static native long allocateMaskBufferPtr(int size, int regionSize);

  private static native long allocateVertexBufferPtr(int size);
}































