#include "AYResource/Converter/GLTFConverter.h"

namespace ayt::resource {

GLTFConverter::GLTFConverter() = default;
GLTFConverter::GLTFConverter(const std::string& path) : sourcePath(path) {}

void GLTFConverter::setSourcePath(const std::string& path) { sourcePath = path; }
void GLTFConverter::setOutputDir(const std::string& dir) { outputDir = dir; }

ConversionResult GLTFConverter::convert()
{
    _pipeline.setSourcePath(sourcePath);
    _pipeline.setOutputDir(outputDir);
    _pipeline.setLoadOption(loadOption);
    return _pipeline.convert();
}

} // namespace ayt::resource