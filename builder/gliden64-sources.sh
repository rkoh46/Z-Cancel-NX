# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by builder/build-switch.sh and builder/build-mac.sh: the files of RMG-K's GLideN64
# that make the plugin, relative to mupen64plus-video-GLideN64/src.
#
# GLideN64's CMake list for a mupen64plus plugin on Unix, with: CRC32_ARMV8.cpp for
# CRC32.cpp (its CRC_ARMV8 option), TextDrawerStub.cpp for TextDrawer.cpp (NO_OSD),
# TxFilterStub.cpp for the GLideNHQ library (NOHQ), mupenplus/Config_mupenplus.cpp for the
# Qt dialog (MUPENPLUSAPI_GLIDENUI off), and its osal library's two files.
GLN_SOURCES="
3DMath.cpp Combiner.cpp CombinerKey.cpp CommonPluginAPI.cpp Config.cpp convert.cpp CRC32_ARMV8.cpp DebugDump.cpp
Debugger.cpp DepthBuffer.cpp DisplayWindow.cpp DisplayLoadProgress.cpp FrameBuffer.cpp FrameBufferInfo.cpp
GBI.cpp gDP.cpp GLideN64.cpp GraphicsDrawer.cpp gSP.cpp Log.cpp N64.cpp PaletteTexture.cpp Performance.cpp
PostProcessor.cpp RDP.cpp RSP.cpp RSP_LoadMatrix.cpp SoftwareRender.cpp TexrectDrawer.cpp TextDrawerStub.cpp
TextureFilterHandler.cpp Textures.cpp VI.cpp ZlutTexture.cpp TxFilterStub.cpp
BufferCopy/BlueNoiseTexture.cpp BufferCopy/ColorBufferToRDRAM.cpp BufferCopy/DepthBufferToRDRAM.cpp
BufferCopy/RDRAMtoColorBuffer.cpp DepthBufferRender/ClipPolygon.cpp DepthBufferRender/DepthBufferRender.cpp
common/CommonAPIImpl_common.cpp Graphics/Context.cpp Graphics/ColorBufferReader.cpp Graphics/CombinerProgram.cpp
Graphics/ObjectHandle.cpp Graphics/OpenGLContext/GLFunctions.cpp
Graphics/OpenGLContext/ThreadedOpenGl/opengl_Command.cpp Graphics/OpenGLContext/ThreadedOpenGl/opengl_ObjectPool.cpp
Graphics/OpenGLContext/ThreadedOpenGl/opengl_Wrapper.cpp Graphics/OpenGLContext/ThreadedOpenGl/opengl_WrappedFunctions.cpp
Graphics/OpenGLContext/ThreadedOpenGl/RingBufferPool.cpp Graphics/OpenGLContext/opengl_Attributes.cpp
Graphics/OpenGLContext/opengl_BufferedDrawer.cpp Graphics/OpenGLContext/opengl_BufferManipulationObjectFactory.cpp
Graphics/OpenGLContext/opengl_CachedFunctions.cpp Graphics/OpenGLContext/opengl_ColorBufferReaderWithBufferStorage.cpp
Graphics/OpenGLContext/opengl_ColorBufferReaderWithPixelBuffer.cpp Graphics/OpenGLContext/opengl_ColorBufferReaderWithReadPixels.cpp
Graphics/OpenGLContext/opengl_ContextImpl.cpp Graphics/OpenGLContext/opengl_GLInfo.cpp Graphics/OpenGLContext/opengl_Parameters.cpp
Graphics/OpenGLContext/opengl_TextureManipulationObjectFactory.cpp Graphics/OpenGLContext/opengl_UnbufferedDrawer.cpp
Graphics/OpenGLContext/opengl_Utils.cpp Graphics/OpenGLContext/GLSL/glsl_CombinerInputs.cpp
Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilder.cpp Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderCommon.cpp
Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderAccurate.cpp Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderFast.cpp
Graphics/OpenGLContext/GLSL/glsl_CombinerProgramImpl.cpp Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactory.cpp
Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactoryCommon.cpp
Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactoryAccurate.cpp
Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactoryFast.cpp Graphics/OpenGLContext/GLSL/glsl_FXAA.cpp
Graphics/OpenGLContext/GLSL/glsl_ShaderStorage.cpp Graphics/OpenGLContext/GLSL/glsl_SpecialShadersFactory.cpp
Graphics/OpenGLContext/GLSL/glsl_Utils.cpp
uCodes/F3D.cpp uCodes/F3DBETA.cpp uCodes/F3DDKR.cpp uCodes/F3DEX.cpp uCodes/F3DEX095.cpp uCodes/F3DAM.cpp
uCodes/F3DEX2.cpp uCodes/F3DEX3.cpp uCodes/F3DEX2ACCLAIM.cpp uCodes/F3DEX2CBFD.cpp uCodes/F3DZEX2.cpp
uCodes/F3DFLX2.cpp uCodes/F3DGOLDEN.cpp uCodes/F3DTEXA.cpp uCodes/F3DPD.cpp uCodes/F3DSETA.cpp
uCodes/F5Indi_Naboo.cpp uCodes/F5Rogue.cpp uCodes/L3D.cpp uCodes/L3DEX2.cpp uCodes/L3DEX.cpp uCodes/S2DEX2.cpp
uCodes/S2DEX.cpp uCodes/T3DUX.cpp uCodes/Turbo3D.cpp uCodes/ZSort.cpp uCodes/ZSortBOSS.cpp
MupenPlusPluginAPI.cpp mupenplus/CommonAPIImpl_mupenplus.cpp mupenplus/MemoryStatus_mupenplus.cpp
mupenplus/MupenPlusAPIImpl.cpp mupenplus/Config_mupenplus.cpp
Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp
osal/osal_files_unix.c osal/osal_keys_unix.c
"
