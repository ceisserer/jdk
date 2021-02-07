 
/*
 * Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef HEADLESS

#include <stdlib.h>
#include <string.h>

#include "sun_java2d_SunGraphics2D.h"

#include "OGLPaints.h"
#include "OGLMaskBuffer.h"


#define GL_SHADER_STORAGE_BUFFER 0x90D2

static int maskBufferPos;
static GLuint maskBufferID; 
static unsigned char* maskBuffer;

static int vtxBufferSize;
static int vtxPerArea;

static   GLuint VboID;
static int lastVertPos;
static int curVertPos;
static GLfloat* vtxPosAndSource;
static GLuint* tileDataVtx;

static GLuint maskFillProg;
static GLuint transform_location;

static jmethodID setFenceAvailableId;
static jclass maskBufferCls;

static GLsync vtxSyncs[3];
static GLsync maskSyncs[4];

void
OGLMaskBuffer_FlushMaskCache()
{
    J2dTraceLn(J2D_TRACE_INFO, "OGLMaskBuffer_FlushMaskBuffer");

    int vertexCnt = curVertPos - lastVertPos;

    if(vertexCnt > 0) {
        
        // We've just crossed a region boundary (1->2)
        // Area 1 is now full (place fence)
        // Area 2 is now written to
        
       // printf("DrawArrays: %d, %d\n", lastVertPos, vertexCnt);
        j2d_glDrawArrays(GL_QUADS, lastVertPos, vertexCnt);
        
        if(curVertPos == vtxBufferSize) {
            curVertPos = 0;
        }
        
        lastVertPos = curVertPos;
    }
}


void OGLMaskBuffer_QueueMaskBufferFence(JNIEnv *env, OGLContext *oglc, jint fenceRegion, jint waitRegion) {
    
    OGLMaskBuffer_FlushMaskCache();
    maskSyncs[fenceRegion] = j2d_glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
    
    j2d_glFlush();
    
    if(waitRegion >= 0) {
        waitForFence(maskSyncs[waitRegion]);
        maskSyncs[waitRegion] = NULL;
        
       // printf("calling available with %d\n", waitRegion);
        (*env)->CallStaticVoidMethod(env, maskBufferCls, setFenceAvailableId, waitRegion);
    }
}


JNIEXPORT jlong JNICALL
Java_sun_java2d_opengl_OGLMaskBuffer_allocateMaskBufferPtr(JNIEnv *env, jclass cls, jint size) {
  
  const char gVertexShaderSource[] = {
                         "#version 130\n"
                         ""
                         "in vec4 posAndSource;\n"
                         "in uvec4 tileDataVtx;\n"
                         ""
                         "uniform mat4 transform;\n"
                         ""
                         "flat out uvec2 boundingBoxScreenFrag;\n"
                         "flat out uvec2 maksTilePosFrag;\n"
                         "flat out vec4 colorFrag;\n"
                         ""
                         "void main(void)\n"
                         "{\n"
                         "  vec2 position = posAndSource.xy;\n"
                         "  vec2 sourceInfo = posAndSource.zw;\n"
                         "  boundingBoxScreenFrag = tileDataVtx.xy; \n"
                         "  maksTilePosFrag = tileDataVtx.zw; \n"
                         "  uint rg = uint(sourceInfo.x); \n"
                         "  uint ba = uint(sourceInfo.y); \n"
                         "  float r = rg & uint(0xFF); \n"
                         "  float g = (rg >> 8) & uint(0xFF); \n"
                         "  float b = ba & uint(0xFF); \n"
                         "  float a = (ba >> 8) & uint(0xFF); \n"
                         "  colorFrag = vec4(r,g,b,a) / 255; \n"
                         "  gl_Position = transform * vec4(position, 0.0, 1.0);\n"
                         "}\n" 
                         };
                         
  const char gFragmentShaderSource[] = {
                          "%s"
                         "#extension GL_ARB_shader_storage_buffer_object : enable\n"
                         "#extension GL_ARB_fragment_coord_conventions : enable\n"
                         "layout (%s binding = 0) buffer position {\n"
                         "  uint[] mask;"
                         "};"
                         "layout(origin_upper_left) in vec4 gl_FragCoord;"
                         ""
                         "uniform sampler2D texture;"
                         ""
                         "flat in uvec2 boundingBoxScreenFrag;"
                         "flat in uvec2 maksTilePosFrag;" //x = offset in ssbo in bytes, y = width of the mask in pixels
                         "flat in vec4 colorFrag;"
                         "" 
                         "out vec4 color;\n"          
                         "void main(void)\n"
                         "{\n"
                         "   float maskVal = 1.0;"
                         ""
                         "  if(maksTilePosFrag.x != uint(0x7FFFFFFF)) {"
                         "    uvec2 fragmentScreenPos = uvec2(gl_FragCoord.x, gl_FragCoord.y); \n"
                         "    uvec2 relativePosInBoundingBox = fragmentScreenPos - boundingBoxScreenFrag; \n"
                         "    uint byteOffset = maksTilePosFrag.x + relativePosInBoundingBox.y * maksTilePosFrag.y + relativePosInBoundingBox.x;"
                         "    uint arrayVal = mask[byteOffset / uint(4)]; \n"
                         "    uint byteShift = ((byteOffset %s uint(4)) * uint(8));"
                         "    uint grayVal = (arrayVal >> byteShift) & uint(0xFF); \n" 
                         "    maskVal = grayVal / 255.0; \n"
                         "  }"
                         ""
                         "  color = colorFrag * maskVal;"
                         "}\n" 
                         };
                         
                         char finalFragSource[4000];
                         memset(finalFragSource, 0, 4000);
                    
                        const GLubyte* glVersion = j2d_glGetString(GL_VERSION);
                        int major = glVersion[0] - '0';
                        int minor = glVersion[2] - '0';  
                        
                        
                         
                         if(major >= 4 || (major >= 3 && minor >= 1)) {
                            sprintf(finalFragSource, gFragmentShaderSource, "#version 140\n", "std430,", "%");  
                            printf("GLSL 140 path\n");                 
                          } 
                        else if(major >= 3) 
                        {
                            sprintf(finalFragSource, gFragmentShaderSource, "#version 130\n", "", "%");  
                            printf("GLSL 130 path\n");
                        } else {
                            printf("GL too old!\n");
                        }
                                          
        maskFillProg = OGLContext_CreateProgram(gVertexShaderSource, finalFragSource);
    if (maskFillProg == 0) {
        printf("error!!!\n");
        J2dRlsTraceLn(J2D_TRACE_ERROR,
            "OGLPaints_CreateMultiGradProgram: error creating program");
        return 0;
    }
    
    transform_location = j2d_glGetUniformLocationARB(maskFillProg, "transform");


   GLint stride;
   GLenum prop = GL_ARRAY_STRIDE;
   j2d_glGetProgramResourceiv(maskFillProg, GL_BUFFER_VARIABLE, 0, 1, &prop, 1, NULL, &stride);
    stride  /= 4;

  jfieldID jFieldId = (*env)->GetStaticFieldID(env, cls, "BUFFER_ARRAY_STRIDE","I");
  setFenceAvailableId = (*env)->GetStaticMethodID(env, cls, "setFenceAvailable", "(I)V");
  printf("MID: %d\n", setFenceAvailableId);
  maskBufferCls = cls;
  
  (*env)->SetStaticIntField(env,cls,jFieldId,stride);  

    
    
  j2d_glGenBuffers( 1, &maskBufferID );
  j2d_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, maskBufferID);
  j2d_glBindBuffer(GL_SHADER_STORAGE_BUFFER, maskBufferID );

  //Create an immutable data store for the buffer
  size_t bufferSize = size; //1024 * 1024 * 4;
  GLbitfield flags = GL_MAP_WRITE_BIT | 
                               GL_MAP_PERSISTENT_BIT |
                               GL_MAP_COHERENT_BIT;
                               

                               
  j2d_glBufferStorage(GL_SHADER_STORAGE_BUFFER, bufferSize, 0, flags);
  maskBuffer = (unsigned char*) j2d_glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bufferSize, flags ); 
  
  printf("Buffer address from java: %d\n", maskBuffer);
  
  maskBufferPos = 0;
  
  return (jlong) maskBuffer;
  
  return 0;
}

JNIEXPORT jlong JNICALL
Java_sun_java2d_opengl_OGLMaskBuffer_allocateVertexBufferPtr(JNIEnv *env, jobject self, jint size) {
  //Vertex-Data per Mask-Quad: 1 Vertex = 8*sizeof(float) = 32 byte
  vtxBufferSize = size / 32;
  vtxPerArea = vtxBufferSize / 3;

  vtxPerArea -= vtxPerArea % 4;
  vtxBufferSize = vtxPerArea * 3;

  size = vtxBufferSize * 32;
    
  j2d_glGenBuffers(1, &VboID); 
  j2d_glBindBuffer(GL_ARRAY_BUFFER, VboID); 
  GLbitfield vertFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
  j2d_glBufferStorage( GL_ARRAY_BUFFER, size, 0, vertFlags);
  unsigned char* vertexBuffer = (unsigned char*) j2d_glMapBufferRange( GL_ARRAY_BUFFER, 0, size, vertFlags );
  vtxPosAndSource = (GLfloat*) vertexBuffer;
  tileDataVtx = (GLuint*) &vertexBuffer[size / 2];
  curVertPos = 0;
  lastVertPos = 0;
  
  return (jlong) vtxPosAndSource;
}

extern float* orthoPtr;

void
OGLMaskBuffer_EnableMaskBuffer(OGLContext *oglc)
{
    J2dTraceLn(J2D_TRACE_INFO, "OGLVertexCache_EnableMaskCache");


    if(maskFillProg == 0) {
            printf("Program not initialized\n");
    }

    
  j2d_glUseProgramObjectARB(maskFillProg);
    
  j2d_glBindBuffer(GL_SHADER_STORAGE_BUFFER, maskBufferID );
  j2d_glBindBuffer(GL_ARRAY_BUFFER, VboID); 

  j2d_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
  j2d_glVertexAttribIPointer(1, 4, GL_UNSIGNED_INT, 0, (const void*) (vtxBufferSize * 32 / 2));
  
  j2d_glEnableVertexAttribArray(0);
  j2d_glEnableVertexAttribArray(1);

  j2d_glUniformMatrix4fv(transform_location, 1, GL_FALSE, orthoPtr);
}


void
OGLMaskBuffer_DisableMaskBuffer(OGLContext *oglc)
{
    J2dTraceLn(J2D_TRACE_INFO, "OGLMaskBUffer_DisableMaskCache");

    OGLMaskBuffer_FlushMaskCache();
   
   j2d_glDisableVertexAttribArray(1); 
   j2d_glDisableVertexAttribArray(0);
   j2d_glUseProgramObjectARB(0);
}


void waitForFence(GLsync sync) {
    GLenum waitReturn;
    do {
        waitReturn = j2d_glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
    } while( waitReturn != GL_ALREADY_SIGNALED && waitReturn != GL_CONDITION_SATISFIED);
    
    j2d_glDeleteSync( sync );
}


void queueMaskQuad(int dstX, int dstY, int w, int h, int maskOffset, int r, int g, int b, int a, int setColor) {
    // We'll cross region boundary with this quad, so make sure the GPU is done with the vtx data previsouly stored here
    // TODO: Make sure region size aligns perfectly with quad vertex count (4), so we can do the check below
    if(curVertPos % vtxPerArea == 0) {
        //if(curVertPos == vtxBufferSize) 
        {
            OGLMaskBuffer_FlushMaskCache();
        }
        
        int nextRegion = curVertPos / vtxPerArea;
        int lastRegion = (nextRegion + 2) % 3;
        
       // printf("Fence was placed: %d, %d\n",lastRegion, nextRegion );
        
        if(vtxSyncs[lastRegion] != NULL) {
            printf("Sync was in progress! %d\n", lastRegion);
        }
        
        vtxSyncs[lastRegion] = j2d_glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
        
        
        GLsync nextSync = vtxSyncs[nextRegion];
        if(nextSync != NULL) {
           // printf("waiting for fence, vtxPer: %d, %d\n", nextRegion, vtxPerArea); 
            
            waitForFence(nextSync);
            vtxSyncs[nextRegion] = NULL;
        }
    }
    
    int dstX2 = dstX + w;
    int dstY2 = dstY + h;
        
    // 1 Quad -> 4 points
    GLfloat* vert = &vtxPosAndSource[4 * curVertPos];
    vert[0] = dstX;
    vert[1] = dstY;
    vert[4] = dstX2;
    vert[5] = dstY;
    vert[8] = dstX2;
    vert[9] = dstY2;
    vert[12] = dstX;
    vert[13] = dstY2;
    
    // we are using "flat" interpolation 
    // -> set attributes only for the "provoking" vertex (last vertex of a quad)
    
    if(setColor) {
        vert[14] = (g << 8) + r;
        vert[15] = (a << 8) + b;
    }

    GLuint* tileData = &tileDataVtx[curVertPos * 4];
    tileData[12] = dstX;
    tileData[13] = dstY;       
    tileData[14] = maskOffset;
    tileData[15] = w;
  
    curVertPos += 4;
}



void
OGLMaskBuffer_AddMaskQuadTurbo(OGLContext *oglc,
                           jint dstx, jint dsty,
                           jint w, jint h, jint maskOffset) {

    queueMaskQuad(dstx, dsty, w, h, maskOffset, oglc->r, oglc->g, oglc->b, oglc->a, 1);    
}

#endif /* !HEADLESS */
