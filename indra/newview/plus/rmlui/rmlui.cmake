# -*- cmake -*-
# plus/rmlui: windows whose content is an RmlUi document (HTML and CSS-like files), drawn through the official
# UI path (research/364). Included by plus/plus.cmake.
#
# thirdparty/RmlUi-6.3/ holds RmlUi 6.3's Include/ and Source/Core/ as released (ORIGIN.md, SHA256SUMS). Its own
# CMakeLists.txt is not used: Source/Core (with the FreeType font engine) is built here as the static library
# plus_rmlui_core. No debugger, no Lua, no SVG or Lottie plugin, and no default file interface: documents are
# read only through rmlsystem.cpp's, which keeps to app_settings/plus/rmlui/.
# The executable is made after this file (indra/newview/CMakeLists.txt), so its link to the library is
# deferred to the end of the folder. Re-run CMake configure after a change of these lists.

include(FreeType)

set(plus_RMLUI_CORE_SOURCE_FILES
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/BaseXMLParser.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Box.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/BoxShadowCache.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/CallbackTexture.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Clock.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/CompiledFilterShader.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ComputeProperty.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ComputedValues.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Context.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ContextInstancer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ContextInstancerDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ConvolutionFilter.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Core.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataController.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataControllerDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataExpression.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataModel.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataModelHandle.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataTypeRegister.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataVariable.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataView.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DataViewDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Decorator.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorGradient.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorNinePatch.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorShader.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorText.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorTiled.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorTiledBox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorTiledHorizontal.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorTiledImage.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorTiledVertical.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DecoratorUtilities.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/DocumentHeader.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/EffectSpecification.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Element.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementAnimation.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementBackgroundBorder.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementDefinition.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementDocument.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementEffects.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementHandle.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementInstancer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementMeta.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementScroll.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementStyle.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementText.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ElementUtilities.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementForm.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementFormControl.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementFormControlInput.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementFormControlSelect.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementFormControlTextArea.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementImage.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementLabel.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementProgress.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementTabSet.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/ElementTextSelection.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputType.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputTypeButton.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputTypeCheckbox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputTypeRadio.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputTypeRange.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputTypeSubmit.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/InputTypeText.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/WidgetDropDown.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/WidgetSlider.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/WidgetTextInput.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/WidgetTextInputMultiLine.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/WidgetTextInputSingleLine.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/WidgetTextInputSingleLinePassword.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/XMLNodeHandlerSelect.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/XMLNodeHandlerTabSet.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Elements/XMLNodeHandlerTextArea.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Event.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/EventDispatcher.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/EventInstancer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/EventInstancerDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/EventListenerInstancer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/EventSpecification.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Factory.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FileInterface.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FileInterfaceDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Filter.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FilterBasic.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FilterBlur.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FilterDropShadow.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEffect.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEffectBlur.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEffectGlow.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEffectInstancer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEffectOutline.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEffectShadow.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FontEngineInterfaceDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FontFace.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FontFaceHandleDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FontFaceLayer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FontFamily.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FontProvider.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineDefault/FreeTypeInterface.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/FontEngineInterface.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Geometry.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/GeometryBackgroundBorder.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/GeometryBoxShadow.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/BlockContainer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/BlockFormattingContext.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/ContainerBox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/FlexFormattingContext.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/FloatedBoxSpace.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/FormattingContext.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/InlineBox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/InlineContainer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/InlineLevelBox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/LayoutBox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/LayoutDetails.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/LayoutEngine.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/LayoutPools.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/LineBox.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/ReplacedFormattingContext.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/TableFormattingContext.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Layout/TableFormattingDetails.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Log.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/LogDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Math.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Memory.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/MeshUtilities.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ObserverPtr.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Plugin.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PluginRegistry.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Profiling.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertiesIteratorView.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Property.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyDefinition.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyDictionary.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserAnimation.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserBoxShadow.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserColorStopList.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserColour.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserDecorator.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserFilter.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserFontEffect.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserKeyword.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserNumber.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserRatio.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserString.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertyParserTransform.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/PropertySpecification.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/RenderInterface.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/RenderInterfaceCompatibility.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/RenderManager.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/RenderManagerAccess.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ScriptInterface.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/ScrollController.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Spritesheet.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Stream.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StreamFile.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StreamMemory.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StringUtilities.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheet.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheetContainer.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheetFactory.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheetNode.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheetParser.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheetSelector.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/StyleSheetSpecification.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/SystemInterface.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Template.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TemplateCache.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Texture.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TextureDatabase.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TextureLayout.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TextureLayoutRectangle.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TextureLayoutRow.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TextureLayoutTexture.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Traits.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Transform.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TransformPrimitive.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TransformState.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TransformUtilities.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Tween.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/TypeConverter.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/URL.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/Variant.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/WidgetScroll.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLNodeHandler.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLNodeHandlerBody.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLNodeHandlerDefault.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLNodeHandlerHead.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLNodeHandlerTemplate.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLParseTools.cpp
    plus/rmlui/thirdparty/RmlUi-6.3/Source/Core/XMLParser.cpp
    )

add_library(plus_rmlui_core STATIC ${plus_RMLUI_CORE_SOURCE_FILES})
target_compile_definitions(plus_rmlui_core
    PUBLIC RMLUI_STATIC_LIB
    PRIVATE RMLUI_FONT_ENGINE_FREETYPE RMLUI_NO_FILE_INTERFACE_DEFAULT "RMLUI_VERSION=\"6.3\"")
target_link_libraries(plus_rmlui_core PRIVATE ll::freetype)
if (WINDOWS)
    # /fp:precise for RmlUi: it compares with infinity (Context::GetNextUpdateDelay, the layout's unbounded
    # sizes), which /fp:fast (00-Common.cmake) lets the compiler assume away.
    get_target_property(plus_rmlui_core_options plus_rmlui_core COMPILE_OPTIONS)
    list(REMOVE_ITEM plus_rmlui_core_options /fp:fast)
    list(APPEND plus_rmlui_core_options /fp:precise)
    set_target_properties(plus_rmlui_core PROPERTIES COMPILE_OPTIONS "${plus_rmlui_core_options}")
endif (WINDOWS)

# CMake version: indra/CMakeLists.txt declares cmake_minimum_required(VERSION 3.16.0...4.0), but this file needs
# 3.19 (cmake_language(DEFER), the link made after add_executable). With plus/rmlui in the tree the viewer
# therefore needs CMake 3.19 or newer in fact; the build uses Visual Studio 2022's CMake (3.31.6 on the build PC).
# Whether to raise the declaration itself is the main session's decision (research/364, the review fixes'
# handoff); until then an older CMake stops here with this message rather than in an unknown command.
if (CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR "plus/rmlui needs CMake 3.19 or newer (cmake_language DEFER); indra/CMakeLists.txt still declares 3.16")
endif ()
cmake_language(DEFER CALL target_link_libraries ${VIEWER_BINARY_NAME} plus_rmlui_core)

list(APPEND plus_SOURCE_FILES
    plus/rmlui/rmlcolour.cpp
    plus/rmlui/rmldemo.cpp
    plus/rmlui/rmlfloater.cpp
    plus/rmlui/rmlfonts.cpp
    plus/rmlui/rmlhost.cpp
    plus/rmlui/rmlime.cpp
    plus/rmlui/rmlrender.cpp
    plus/rmlui/rmlsettings.cpp
    plus/rmlui/rmlsystem.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/rmlui/rmlcolour.h
    plus/rmlui/rmlcommon.h
    plus/rmlui/rmldemo.h
    plus/rmlui/rmlfloater.h
    plus/rmlui/rmlfonts.h
    plus/rmlui/rmlhost.h
    plus/rmlui/rmlime.h
    plus/rmlui/rmlrender.h
    plus/rmlui/rmlsettings.h
    plus/rmlui/rmlsystem.h
    )