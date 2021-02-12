/*
 * Copyright (c) 2003, 2007, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package sun.java2d.opengl;

import java.awt.*;

import sun.java2d.InvalidPipeException;
import sun.java2d.SunGraphics2D;
import sun.java2d.SurfaceData;
import sun.java2d.loops.GraphicsPrimitive;
import sun.java2d.loops.GraphicsPrimitiveMgr;
import sun.java2d.loops.CompositeType;
import sun.java2d.loops.SurfaceType;
import sun.java2d.pipe.BufferedContext;
import sun.java2d.pipe.BufferedMaskFill;
import sun.java2d.pipe.RenderBuffer;

import static sun.java2d.loops.CompositeType.*;
import static sun.java2d.loops.SurfaceType.*;
import static sun.java2d.pipe.BufferedOpCodes.*;

class OGLMaskFill extends BufferedMaskFill {

    static void register() {
        GraphicsPrimitive[] primitives = {
            new OGLMaskFill(AnyColor,                  SrcOver),
            new OGLMaskFill(OpaqueColor,               SrcNoEa),
            new OGLMaskFill(GradientPaint,             SrcOver),
            new OGLMaskFill(OpaqueGradientPaint,       SrcNoEa),
            new OGLMaskFill(LinearGradientPaint,       SrcOver),
            new OGLMaskFill(OpaqueLinearGradientPaint, SrcNoEa),
            new OGLMaskFill(RadialGradientPaint,       SrcOver),
            new OGLMaskFill(OpaqueRadialGradientPaint, SrcNoEa),
            new OGLMaskFill(TexturePaint,              SrcOver),
            new OGLMaskFill(OpaqueTexturePaint,        SrcNoEa),
        };
        GraphicsPrimitiveMgr.register(primitives);
    }

    protected OGLMaskFill(SurfaceType srcType, CompositeType compType) {
        super(OGLRenderQueue.getInstance(),
              srcType, compType, OGLSurfaceData.OpenGLSurface);
    }

    @Override
    protected native void maskFill(int x, int y, int w, int h,
                                   int maskoff, int maskscan, int masklen,
                                   byte[] mask);

    @Override
    protected void validateContext(SunGraphics2D sg2d,
                                   Composite comp, int ctxflags)
    {
        OGLSurfaceData dstData;
        try {
            dstData = (OGLSurfaceData) sg2d.surfaceData;
        } catch (ClassCastException e) {
            throw new InvalidPipeException("wrong surface data type: " +
                                           sg2d.surfaceData);
        }

        OGLContext.validateContext(dstData, dstData,
                                   sg2d.getCompClip(), comp,
                                   null, sg2d.paint, sg2d, ctxflags);
    }


  /*@Override
  public void MaskFill(SunGraphics2D sg2d, SurfaceData sData,
                       Composite comp,
                       final int x, final int y, final int w, final int h,
                       final byte[] mask,
                       final int maskoff, final int maskscan)
  {
    OGLMaskBuffer.getInstance();
    super.MaskFill(sg2d, sData, comp, x, y, w, h, mask, maskoff, maskscan);
  }*/


  @Override
  public void MaskFill(SunGraphics2D sg2d, SurfaceData sData,
                       Composite comp,
                       final int x, final int y, final int w, final int h,
                       final byte[] mask,
                       final int maskoff, final int maskscan)
  {
    AlphaComposite acomp = (AlphaComposite)comp;
    if (acomp.getRule() != AlphaComposite.SRC_OVER) {
      comp = AlphaComposite.SrcOver;
    }

    rq.lock();
    try {
      validateContext(sg2d, comp, BufferedContext.USE_MASK);

      rq.ensureCapacity(24);
      RenderBuffer buf = rq.getBuffer();

      OGLMaskBuffer maskBuffer = OGLMaskBuffer.getInstance();

      int maskOffset = Integer.MAX_VALUE;
      if(mask != null) {
        maskOffset = maskBuffer.allocateMaskData(rq, w*h);

        // Just to illustrate how to write to VRAM
        long maskBuffPtr = maskBuffer.getMaskBufferBasePtr() + maskOffset;
        for (int i = 0; i < h; i++) {
          for (int m = 0; m < w; m++) {
            byte source = mask[maskoff + maskscan * i + m];
            //System.out.println(source);
            if(source != 0) {
              OGLMaskBuffer.UNSAFE.putByte(maskBuffPtr, source);
            }
            maskBuffPtr++;
          }
        }
      }

      buf = rq.getBuffer();
      buf.putInt(TURBO_MASK_FILL);
      // enqueue parameters
      buf.putInt(x).putInt(y).putInt(w).putInt(h);
      buf.putInt(maskOffset);

    } finally {
      rq.unlock();
    }
  }
}
