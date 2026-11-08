# -*- cmake -*-
# The sky and water uniforms sent again only when they change
# (LLRHI_ENV_UNIFORM_MEMO), research/187 N4 in the sl-viewer repository.
# Included by plus/plus.cmake. Hooks: LLEnvironment::update,
# LLEnvironment::updateShaderUniforms; llrender: LLGLSLShader::bind,
# uniformChanged, forgetUniform, mapUniforms, dropNativeProgram.

list(APPEND plus_SOURCE_FILES
    plus/envmemo/envmemo.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/envmemo/envmemo.h
    )
