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

  private static final Unsafe UNSAFE;

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
    maskBufferBasePtr = allocateMaskBufferPtr(MASK_BUFFER_SIZE);

    currentVtxPos = 0;
    lastVtxPos = 0;
    currentBufferOffset = 0;
  }

  public final int queueMaskQuad(RenderQueue queue, int w, int h, byte[] mask, int maskScan, int maskOff) {
    int offsetBefore = currentBufferOffset;

    if(mask != null) {
      int maskSize = w * h;

      int regionBefore = currentBufferOffset / MASK_BUFFER_REGION_SIZE;

      if (currentBufferOffset + maskSize >= MASK_BUFFER_SIZE) {
        offsetBefore = currentBufferOffset = 0;
      }
      long maskBuffPtr = maskBufferBasePtr + currentBufferOffset;

      currentBufferOffset += maskSize;
      int regionAfter = currentBufferOffset / MASK_BUFFER_REGION_SIZE;

      if(regionBefore != regionAfter) {
          queue.ensureCapacity(12);
          RenderBuffer buffer = queue.getBuffer();
          buffer.putInt(MASK_BUFFER_FENCE);
          buffer.putInt(regionBefore);

          boolean nextRegionPending;
        synchronized (pendingFences) {
          int waitRegion = (regionBefore + 2) % MASK_BUFFER_REGION_COUNT;
          if(!pendingFences[waitRegion]) {
            waitRegion = -1;
          }
          buffer.putInt(waitRegion);

            // enable in case async flush is available
          //queue.flushNow(false);

          pendingFences[regionBefore] = true;

          nextRegionPending = pendingFences[regionAfter];
        }

        int fenceCounter = 0;
        while(nextRegionPending) {
        //  System.out.println("We have a real problem!!");
          queue.flushNow();
          synchronized (pendingFences) {
            nextRegionPending = pendingFences[regionAfter];
          }

          if(fenceCounter > 0) {
            System.out.println(fenceCounter);
          }

          fenceCounter++;
        }
      }

      for (int i = 0; i < h; i++) {
        for (int m = 0; m < w; m++) {
          UNSAFE.putByte(maskBuffPtr++, mask[maskOff + maskScan * i + m]);
        }
      }
    }

    return offsetBefore;
  }

  private static void setFenceAvailable(int fenceNum) {
    synchronized (pendingFences) {
     // System.out.println("Fence available: " + fenceNum);
      pendingFences[fenceNum] = false;
    }
  }

  private static native long allocateMaskBufferPtr(int size);

  private static native long allocateVertexBufferPtr(int size);


}































