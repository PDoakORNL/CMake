/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmVisualStudio10TargetGenerator.h"

#include "cmAlgorithms.h"
#include "cmComputeLinkInformation.h"
#include "cmCustomCommandGenerator.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalVisualStudio10Generator.h"
#include "cmLocalVisualStudio7Generator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmSystemTools.h"
#include "cmVisualStudioGeneratorOptions.h"
#include "windows.h"

#include <iterator>
#include <memory> // IWYU pragma: keep

static void ConvertToWindowsSlash(std::string& s);

static std::string cmVS10EscapeXML(std::string arg)
{
  cmSystemTools::ReplaceString(arg, "&", "&amp;");
  cmSystemTools::ReplaceString(arg, "<", "&lt;");
  cmSystemTools::ReplaceString(arg, ">", "&gt;");
  return arg;
}

static std::string cmVS10EscapeQuotes(std::string arg)
{
  cmSystemTools::ReplaceString(arg, "\"", "&quot;");
  return arg;
}

static std::string cmVS10EscapeComment(std::string comment)
{
  // MSBuild takes the CDATA of a <Message></Message> element and just
  // does "echo $CDATA" with no escapes.  We must encode the string.
  // http://technet.microsoft.com/en-us/library/cc772462%28WS.10%29.aspx
  std::string echoable;
  for (char c : comment) {
    switch (c) {
      case '\r':
        break;
      case '\n':
        echoable += '\t';
        break;
      case '"': /* no break */
      case '|': /* no break */
      case '&': /* no break */
      case '<': /* no break */
      case '>': /* no break */
      case '^':
        echoable += '^'; /* no break */
        CM_FALLTHROUGH;
      default:
        echoable += c;
        break;
    }
  }
  return echoable;
}

static bool cmVS10IsTargetsFile(std::string const& path)
{
  std::string const ext = cmSystemTools::GetFilenameLastExtension(path);
  return cmSystemTools::Strucmp(ext.c_str(), ".targets") == 0;
}

static std::string computeProjectFileExtension(cmGeneratorTarget const* t,
                                               const std::string& config)
{
  std::string res;
  res = ".vcxproj";
  std::string lang = t->GetLinkerLanguage(config);
  if (cmGlobalVisualStudioGenerator::TargetIsCSharpOnly(t) ||
      lang == "CSharp") {
    res = ".csproj";
  }
  return res;
}

cmVisualStudio10TargetGenerator::cmVisualStudio10TargetGenerator(
  cmGeneratorTarget* target, cmGlobalVisualStudio10Generator* gg)
  : GeneratorTarget(target)
  , Makefile(target->Target->GetMakefile())
  , Platform(gg->GetPlatformName())
  , Name(target->GetName())
  , GUID(gg->GetGUID(this->Name))
  , GlobalGenerator(gg)
  , LocalGenerator((cmLocalVisualStudio7Generator*)target->GetLocalGenerator())
{
  this->Makefile->GetConfigurations(this->Configurations);
  this->NsightTegra = gg->IsNsightTegra();
  for (int i = 0; i < 4; ++i) {
    this->NsightTegraVersion[i] = 0;
  }
  sscanf(gg->GetNsightTegraVersion().c_str(), "%u.%u.%u.%u",
         &this->NsightTegraVersion[0], &this->NsightTegraVersion[1],
         &this->NsightTegraVersion[2], &this->NsightTegraVersion[3]);
  this->MSTools = !this->NsightTegra;
  this->Managed = false;
  this->TargetCompileAsWinRT = false;
  this->BuildFileStream = 0;
  this->IsMissingFiles = false;
  this->DefaultArtifactDir =
    this->LocalGenerator->GetCurrentBinaryDirectory() + std::string("/") +
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
  this->InSourceBuild =
    (strcmp(this->Makefile->GetCurrentSourceDirectory(),
            this->Makefile->GetCurrentBinaryDirectory()) == 0);
}

cmVisualStudio10TargetGenerator::~cmVisualStudio10TargetGenerator()
{
  if (!this->BuildFileStream) {
    return;
  }
  if (this->BuildFileStream->Close()) {
    this->GlobalGenerator->FileReplacedDuringGenerate(this->PathToProjectFile);
  }
  delete this->BuildFileStream;
}

void cmVisualStudio10TargetGenerator::WritePlatformConfigTag(
  const char* tag, const std::string& config, int indentLevel,
  const char* attribute, const char* end, std::ostream* stream)

{
  if (!stream) {
    stream = this->BuildFileStream;
  }
  stream->fill(' ');
  stream->width(indentLevel * 2);
  (*stream) << ""; // applies indentation
  (*stream) << "<" << tag << " Condition=\"";
  (*stream) << "'$(Configuration)|$(Platform)'=='";
  (*stream) << config << "|" << this->Platform;
  (*stream) << "'";
  // handle special case for 32 bit C# targets
  if (this->ProjectType == csproj && this->Platform == "Win32") {
    (*stream) << " Or ";
    (*stream) << "'$(Configuration)|$(Platform)'=='";
    (*stream) << config << "|x86";
    (*stream) << "'";
  }
  (*stream) << "\"";
  if (attribute) {
    (*stream) << attribute;
  }
  // close the tag
  (*stream) << ">";
  if (end) {
    (*stream) << end;
  }
}

void cmVisualStudio10TargetGenerator::WriteString(const char* line,
                                                  int indentLevel)
{
  this->BuildFileStream->fill(' ');
  this->BuildFileStream->width(indentLevel * 2);
  // write an empty string to get the fill level indent to print
  (*this->BuildFileStream) << "";
  (*this->BuildFileStream) << line;
}

#define VS10_CXX_DEFAULT_PROPS "$(VCTargetsPath)\\Microsoft.Cpp.Default.props"
#define VS10_CXX_PROPS "$(VCTargetsPath)\\Microsoft.Cpp.props"
#define VS10_CXX_USER_PROPS                                                   \
  "$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props"
#define VS10_CXX_TARGETS "$(VCTargetsPath)\\Microsoft.Cpp.targets"

#define VS10_CSharp_DEFAULT_PROPS                                             \
  "$(MSBuildExtensionsPath)\\$(MSBuildToolsVersion)\\Microsoft.Common.props"
// This does not seem to exist by default, it's just provided for consistency
// in case users want to have default custom props for C# targets
#define VS10_CSharp_USER_PROPS                                                \
  "$(UserRootDir)\\Microsoft.CSharp.$(Platform).user.props"
#define VS10_CSharp_TARGETS "$(MSBuildToolsPath)\\Microsoft.CSharp.targets"

void cmVisualStudio10TargetGenerator::Generate()
{
  // do not generate external ms projects
  if (this->GeneratorTarget->GetType() == cmStateEnums::INTERFACE_LIBRARY ||
      this->GeneratorTarget->GetProperty("EXTERNAL_MSPROJECT")) {
    return;
  }
  this->ProjectFileExtension = computeProjectFileExtension(
    this->GeneratorTarget, *this->Configurations.begin());
  if (this->ProjectFileExtension == ".vcxproj") {
    this->ProjectType = vcxproj;
    this->Managed = false;
  } else if (this->ProjectFileExtension == ".csproj") {
    this->ProjectType = csproj;
    this->Managed = true;
  }
  // Tell the global generator the name of the project file
  this->GeneratorTarget->Target->SetProperty("GENERATOR_FILE_NAME",
                                             this->Name.c_str());
  this->GeneratorTarget->Target->SetProperty(
    "GENERATOR_FILE_NAME_EXT", this->ProjectFileExtension.c_str());
  if (this->GeneratorTarget->GetType() <= cmStateEnums::OBJECT_LIBRARY) {
    if (!this->ComputeClOptions()) {
      return;
    }
    if (!this->ComputeRcOptions()) {
      return;
    }
    if (!this->ComputeCudaOptions()) {
      return;
    }
    if (!this->ComputeCudaLinkOptions()) {
      return;
    }
    if (!this->ComputeMasmOptions()) {
      return;
    }
    if (!this->ComputeNasmOptions()) {
      return;
    }
    if (!this->ComputeLinkOptions()) {
      return;
    }
    if (!this->ComputeLibOptions()) {
      return;
    }
  }
  std::string path = this->LocalGenerator->GetCurrentBinaryDirectory();
  path += "/";
  path += this->Name;
  path += this->ProjectFileExtension;
  this->BuildFileStream = new cmGeneratedFileStream(path.c_str());
  this->PathToProjectFile = path;
  this->BuildFileStream->SetCopyIfDifferent(true);

  // Write the encoding header into the file
  char magic[] = { char(0xEF), char(0xBB), char(0xBF) };
  this->BuildFileStream->write(magic, 3);

  // get the tools version to use
  const std::string toolsVer(this->GlobalGenerator->GetToolsVersion());
  std::string project_defaults = "<?xml version=\"1.0\" encoding=\"" +
    this->GlobalGenerator->Encoding() + "\"?>\n";
  project_defaults.append("<Project DefaultTargets=\"Build\" ToolsVersion=\"");
  project_defaults.append(toolsVer + "\" ");
  project_defaults.append(
    "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n");
  this->WriteString(project_defaults.c_str(), 0);

  if (this->NsightTegra) {
    this->WriteString("<PropertyGroup Label=\"NsightTegraProject\">\n", 1);
    const int nsightTegraMajorVersion = this->NsightTegraVersion[0];
    const int nsightTegraMinorVersion = this->NsightTegraVersion[1];
    if (nsightTegraMajorVersion >= 2) {
      this->WriteString("<NsightTegraProjectRevisionNumber>", 2);
      if (nsightTegraMajorVersion > 3 ||
          (nsightTegraMajorVersion == 3 && nsightTegraMinorVersion >= 1)) {
        (*this->BuildFileStream) << "11";
      } else {
        // Nsight Tegra 2.0 uses project revision 9.
        (*this->BuildFileStream) << "9";
      }
      (*this->BuildFileStream) << "</NsightTegraProjectRevisionNumber>\n";
      // Tell newer versions to upgrade silently when loading.
      this->WriteString("<NsightTegraUpgradeOnceWithoutPrompt>"
                        "true"
                        "</NsightTegraUpgradeOnceWithoutPrompt>\n",
                        2);
    } else {
      // Require Nsight Tegra 1.6 for JCompile support.
      this->WriteString("<NsightTegraProjectRevisionNumber>"
                        "7"
                        "</NsightTegraProjectRevisionNumber>\n",
                        2);
    }
    this->WriteString("</PropertyGroup>\n", 1);
  }

  if (const char* hostArch =
        this->GlobalGenerator->GetPlatformToolsetHostArchitecture()) {
    this->WriteString("<PropertyGroup>\n", 1);
    this->WriteString("<PreferredToolArchitecture>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(hostArch)
                             << "</PreferredToolArchitecture>\n";
    this->WriteString("</PropertyGroup>\n", 1);
  }

  if (this->ProjectType != csproj) {
    this->WriteProjectConfigurations();
  }
  this->WriteString("<PropertyGroup Label=\"Globals\">\n", 1);
  this->WriteString("<ProjectGuid>", 2);
  (*this->BuildFileStream) << "{" << this->GUID << "}</ProjectGuid>\n";

  if (this->MSTools &&
      this->GeneratorTarget->GetType() <= cmStateEnums::GLOBAL_TARGET) {
    this->WriteApplicationTypeSettings();
    this->VerifyNecessaryFiles();
  }

  const char* vsProjectTypes =
    this->GeneratorTarget->GetProperty("VS_GLOBAL_PROJECT_TYPES");
  if (vsProjectTypes) {
    std::string tagName = "ProjectTypes";
    if (this->ProjectType == csproj) {
      tagName = "ProjectTypeGuids";
    }
    this->WriteString("", 2);
    (*this->BuildFileStream) << "<" << tagName << ">"
                             << cmVS10EscapeXML(vsProjectTypes) << "</"
                             << tagName << ">\n";
  }

  const char* vsProjectName =
    this->GeneratorTarget->GetProperty("VS_SCC_PROJECTNAME");
  const char* vsLocalPath =
    this->GeneratorTarget->GetProperty("VS_SCC_LOCALPATH");
  const char* vsProvider =
    this->GeneratorTarget->GetProperty("VS_SCC_PROVIDER");

  if (vsProjectName && vsLocalPath && vsProvider) {
    this->WriteString("<SccProjectName>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(vsProjectName)
                             << "</SccProjectName>\n";
    this->WriteString("<SccLocalPath>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(vsLocalPath)
                             << "</SccLocalPath>\n";
    this->WriteString("<SccProvider>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(vsProvider)
                             << "</SccProvider>\n";

    const char* vsAuxPath =
      this->GeneratorTarget->GetProperty("VS_SCC_AUXPATH");
    if (vsAuxPath) {
      this->WriteString("<SccAuxPath>", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(vsAuxPath)
                               << "</SccAuxPath>\n";
    }
  }

  if (this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_COMPONENT")) {
    this->WriteString("<WinMDAssembly>true</WinMDAssembly>\n", 2);
  }

  const char* vsGlobalKeyword =
    this->GeneratorTarget->GetProperty("VS_GLOBAL_KEYWORD");
  if (!vsGlobalKeyword) {
    this->WriteString("<Keyword>Win32Proj</Keyword>\n", 2);
  } else {
    this->WriteString("<Keyword>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(vsGlobalKeyword)
                             << "</Keyword>\n";
  }

  const char* vsGlobalRootNamespace =
    this->GeneratorTarget->GetProperty("VS_GLOBAL_ROOTNAMESPACE");
  if (vsGlobalRootNamespace) {
    this->WriteString("<RootNamespace>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(vsGlobalRootNamespace)
                             << "</RootNamespace>\n";
  }

  this->WriteString("<Platform>", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(this->Platform)
                           << "</Platform>\n";
  const char* projLabel = this->GeneratorTarget->GetProperty("PROJECT_LABEL");
  if (!projLabel) {
    projLabel = this->Name.c_str();
  }
  this->WriteString("<ProjectName>", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(projLabel) << "</ProjectName>\n";
  if (const char* targetFrameworkVersion = this->GeneratorTarget->GetProperty(
        "VS_DOTNET_TARGET_FRAMEWORK_VERSION")) {
    this->WriteString("<TargetFrameworkVersion>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(targetFrameworkVersion)
                             << "</TargetFrameworkVersion>\n";
  }

  // Disable the project upgrade prompt that is displayed the first time a
  // project using an older toolset version is opened in a newer version of
  // the IDE (respected by VS 2013 and above).
  if (this->GlobalGenerator->GetVersion() >=
      cmGlobalVisualStudioGenerator::VS12) {
    this->WriteString("<VCProjectUpgraderObjectName>NoUpgrade"
                      "</VCProjectUpgraderObjectName>\n",
                      2);
  }

  std::vector<std::string> keys = this->GeneratorTarget->GetPropertyKeys();
  for (std::string const& keyIt : keys) {
    static const char* prefix = "VS_GLOBAL_";
    if (keyIt.find(prefix) != 0)
      continue;
    std::string globalKey = keyIt.substr(strlen(prefix));
    // Skip invalid or separately-handled properties.
    if (globalKey.empty() || globalKey == "PROJECT_TYPES" ||
        globalKey == "ROOTNAMESPACE" || globalKey == "KEYWORD") {
      continue;
    }
    const char* value = this->GeneratorTarget->GetProperty(keyIt);
    if (!value)
      continue;
    this->WriteString("<", 2);
    (*this->BuildFileStream) << globalKey << ">" << cmVS10EscapeXML(value)
                             << "</" << globalKey << ">\n";
  }

  if (this->Managed) {
    std::string outputType = "<OutputType>";
    switch (this->GeneratorTarget->GetType()) {
      case cmStateEnums::OBJECT_LIBRARY:
      case cmStateEnums::STATIC_LIBRARY:
      case cmStateEnums::SHARED_LIBRARY:
        outputType += "Library";
        break;
      case cmStateEnums::MODULE_LIBRARY:
        outputType += "Module";
        break;
      case cmStateEnums::EXECUTABLE:
        if (this->GeneratorTarget->Target->GetPropertyAsBool(
              "WIN32_EXECUTABLE")) {
          outputType += "WinExe";
        } else {
          outputType += "Exe";
        }
        break;
      case cmStateEnums::UTILITY:
      case cmStateEnums::GLOBAL_TARGET:
        outputType += "Utility";
        break;
      case cmStateEnums::UNKNOWN_LIBRARY:
      case cmStateEnums::INTERFACE_LIBRARY:
        break;
    }
    outputType += "</OutputType>\n";
    this->WriteString(outputType.c_str(), 2);
    this->WriteString("<AppDesignerFolder>Properties</AppDesignerFolder>\n",
                      2);
  }

  this->WriteString("</PropertyGroup>\n", 1);

  switch (this->ProjectType) {
    case vcxproj:
      this->WriteString("<Import Project=\"" VS10_CXX_DEFAULT_PROPS "\" />\n",
                        1);
      break;
    case csproj:
      this->WriteString("<Import Project=\"" VS10_CSharp_DEFAULT_PROPS "\" "
                        "Condition=\"Exists('" VS10_CSharp_DEFAULT_PROPS "')\""
                        "/>\n",
                        1);
      break;
  }

  this->WriteProjectConfigurationValues();

  if (this->ProjectType == vcxproj) {
    this->WriteString("<Import Project=\"" VS10_CXX_PROPS "\" />\n", 1);
  }
  this->WriteString("<ImportGroup Label=\"ExtensionSettings\">\n", 1);
  if (this->GlobalGenerator->IsCudaEnabled()) {
    this->WriteString("<Import Project=\"$(VCTargetsPath)\\"
                      "BuildCustomizations\\CUDA ",
                      2);
    (*this->BuildFileStream)
      << cmVS10EscapeXML(this->GlobalGenerator->GetPlatformToolsetCudaString())
      << ".props\" />\n";
  }
  if (this->GlobalGenerator->IsMasmEnabled()) {
    this->WriteString("<Import Project=\"$(VCTargetsPath)\\"
                      "BuildCustomizations\\masm.props\" />\n",
                      2);
  }
  if (this->GlobalGenerator->IsNasmEnabled()) {
    // Always search in the standard modules location.
    std::string propsTemplate =
      GetCMakeFilePath("Templates/MSBuild/nasm.props.in");

    std::string propsLocal;
    propsLocal += this->DefaultArtifactDir;
    propsLocal += "\\nasm.props";
    ConvertToWindowsSlash(propsLocal);
    this->Makefile->ConfigureFile(propsTemplate.c_str(), propsLocal.c_str(),
                                  false, true, true);
    std::string import = std::string("<Import Project=\"") +
      cmVS10EscapeXML(propsLocal) + "\" />\n";
    this->WriteString(import.c_str(), 2);
  }
  this->WriteString("</ImportGroup>\n", 1);
  this->WriteString("<ImportGroup Label=\"PropertySheets\">\n", 1);
  {
    std::string props;
    switch (this->ProjectType) {
      case vcxproj:
        props = VS10_CXX_USER_PROPS;
        break;
      case csproj:
        props = VS10_CSharp_USER_PROPS;
        break;
    }
    if (const char* p = this->GeneratorTarget->GetProperty("VS_USER_PROPS")) {
      props = p;
    }
    if (!props.empty()) {
      ConvertToWindowsSlash(props);
      this->WriteString("", 2);
      (*this->BuildFileStream)
        << "<Import Project=\"" << cmVS10EscapeXML(props) << "\""
        << " Condition=\"exists('" << cmVS10EscapeXML(props) << "')\""
        << " Label=\"LocalAppDataPlatform\" />\n";
    }
  }
  this->WritePlatformExtensions();
  this->WriteString("</ImportGroup>\n", 1);
  this->WriteString("<PropertyGroup Label=\"UserMacros\" />\n", 1);
  this->WriteWinRTPackageCertificateKeyFile();
  this->WritePathAndIncrementalLinkOptions();
  this->WriteItemDefinitionGroups();
  this->WriteCustomCommands();
  this->WriteAllSources();
  this->WriteDotNetReferences();
  this->WriteEmbeddedResourceGroup();
  this->WriteXamlFilesGroup();
  this->WriteWinRTReferences();
  this->WriteProjectReferences();
  this->WriteSDKReferences();
  switch (this->ProjectType) {
    case vcxproj:
      this->WriteString("<Import Project=\"" VS10_CXX_TARGETS "\" />\n", 1);
      break;
    case csproj:
      this->WriteString("<Import Project=\"" VS10_CSharp_TARGETS "\" />\n", 1);
      break;
  }

  this->WriteTargetSpecificReferences();
  this->WriteString("<ImportGroup Label=\"ExtensionTargets\">\n", 1);
  this->WriteTargetsFileReferences();
  if (this->GlobalGenerator->IsCudaEnabled()) {
    this->WriteString("<Import Project=\"$(VCTargetsPath)\\"
                      "BuildCustomizations\\CUDA ",
                      2);
    (*this->BuildFileStream)
      << cmVS10EscapeXML(this->GlobalGenerator->GetPlatformToolsetCudaString())
      << ".targets\" />\n";
  }
  if (this->GlobalGenerator->IsMasmEnabled()) {
    this->WriteString("<Import Project=\"$(VCTargetsPath)\\"
                      "BuildCustomizations\\masm.targets\" />\n",
                      2);
  }
  if (this->GlobalGenerator->IsNasmEnabled()) {
    std::string nasmTargets =
      GetCMakeFilePath("Templates/MSBuild/nasm.targets");
    std::string import = "<Import Project=\"";
    import += cmVS10EscapeXML(nasmTargets) + "\" />\n";
    this->WriteString(import.c_str(), 2);
  }
  this->WriteString("</ImportGroup>\n", 1);
  if (this->ProjectType == csproj) {
    for (std::string const& i : this->Configurations) {
      this->WriteString("<PropertyGroup Condition=\"'$(Configuration)' == '",
                        1);
      (*this->BuildFileStream) << i << "'\">\n";
      this->WriteEvents(i);
      this->WriteString("</PropertyGroup>\n", 1);
    }
    // make sure custom commands are executed before build (if necessary)
    this->WriteString("<PropertyGroup>\n", 1);
    this->WriteString("<BuildDependsOn>\n", 2);
    for (std::string const& i : this->CSharpCustomCommandNames) {
      this->WriteString(i.c_str(), 3);
      (*this->BuildFileStream) << ";\n";
    }
    this->WriteString("$(BuildDependsOn)\n", 3);
    this->WriteString("</BuildDependsOn>\n", 2);
    this->WriteString("</PropertyGroup>\n", 1);
  }
  this->WriteString("</Project>", 0);
  // The groups are stored in a separate file for VS 10
  this->WriteGroups();
}

void cmVisualStudio10TargetGenerator::WriteDotNetReferences()
{
  std::vector<std::string> references;
  typedef std::pair<std::string, std::string> HintReference;
  std::vector<HintReference> hintReferences;
  if (const char* vsDotNetReferences =
        this->GeneratorTarget->GetProperty("VS_DOTNET_REFERENCES")) {
    cmSystemTools::ExpandListArgument(vsDotNetReferences, references);
  }
  cmPropertyMap const& props = this->GeneratorTarget->Target->GetProperties();
  for (auto const& i : props) {
    if (i.first.find("VS_DOTNET_REFERENCE_") == 0) {
      std::string name = i.first.substr(20);
      if (!name.empty()) {
        std::string path = i.second.GetValue();
        if (!cmsys::SystemTools::FileIsFullPath(path)) {
          path = std::string(this->Makefile->GetCurrentSourceDirectory()) +
            "/" + path;
        }
        ConvertToWindowsSlash(path);
        hintReferences.push_back(HintReference(name, path));
      }
    }
  }
  if (!references.empty() || !hintReferences.empty()) {
    this->WriteString("<ItemGroup>\n", 1);
    for (std::string const& ri : references) {
      // if the entry from VS_DOTNET_REFERENCES is an existing file, generate
      // a new hint-reference and name it from the filename
      if (cmsys::SystemTools::FileExists(ri, true)) {
        std::string name = cmsys::SystemTools::GetFilenameWithoutExtension(ri);
        std::string path = ri;
        ConvertToWindowsSlash(path);
        hintReferences.push_back(HintReference(name, path));
      } else {
        this->WriteDotNetReference(ri, "");
      }
    }
    for (const auto& i : hintReferences) {
      this->WriteDotNetReference(i.first, i.second);
    }
    this->WriteString("</ItemGroup>\n", 1);
  }
}

void cmVisualStudio10TargetGenerator::WriteDotNetReference(
  std::string const& ref, std::string const& hint)
{
  this->WriteString("<Reference Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(ref) << "\">\n";
  this->WriteString("<CopyLocalSatelliteAssemblies>true"
                    "</CopyLocalSatelliteAssemblies>\n",
                    3);
  this->WriteString("<ReferenceOutputAssembly>true"
                    "</ReferenceOutputAssembly>\n",
                    3);
  if (!hint.empty()) {
    const char* privateReference = "True";
    if (const char* value = this->GeneratorTarget->GetProperty(
          "VS_DOTNET_REFERENCES_COPY_LOCAL")) {
      if (cmSystemTools::IsOff(value)) {
        privateReference = "False";
      }
    }
    this->WriteString("<Private>", 3);
    (*this->BuildFileStream) << privateReference << "</Private>\n";
    this->WriteString("<HintPath>", 3);
    (*this->BuildFileStream) << hint << "</HintPath>\n";
  }
  this->WriteDotNetReferenceCustomTags(ref);
  this->WriteString("</Reference>\n", 2);
}

void cmVisualStudio10TargetGenerator::WriteDotNetReferenceCustomTags(
  std::string const& ref)
{

  static const std::string refpropPrefix = "VS_DOTNET_REFERENCEPROP_";
  static const std::string refpropInfix = "_TAG_";
  const std::string refPropFullPrefix = refpropPrefix + ref + refpropInfix;
  typedef std::map<std::string, std::string> CustomTags;
  CustomTags tags;
  cmPropertyMap const& props = this->GeneratorTarget->Target->GetProperties();
  for (const auto& i : props) {
    if (i.first.find(refPropFullPrefix) == 0) {
      std::string refTag = i.first.substr(refPropFullPrefix.length());
      std::string refVal = i.second.GetValue();
      if (!refTag.empty() && !refVal.empty()) {
        tags[refTag] = refVal;
      }
    }
  }
  for (auto const& tag : tags) {
    this->WriteString("<", 3);
    (*this->BuildFileStream) << tag.first << ">" << cmVS10EscapeXML(tag.second)
                             << "</" << tag.first << ">\n";
  }
}

void cmVisualStudio10TargetGenerator::WriteEmbeddedResourceGroup()
{
  std::vector<cmSourceFile const*> resxObjs;
  this->GeneratorTarget->GetResxSources(resxObjs, "");
  if (!resxObjs.empty()) {
    this->WriteString("<ItemGroup>\n", 1);
    std::string srcDir = this->Makefile->GetCurrentSourceDirectory();
    ConvertToWindowsSlash(srcDir);
    for (cmSourceFile const* oi : resxObjs) {
      std::string obj = oi->GetFullPath();
      this->WriteString("<EmbeddedResource Include=\"", 2);
      ConvertToWindowsSlash(obj);
      bool useRelativePath = false;
      if (this->ProjectType == csproj && this->InSourceBuild) {
        // If we do an in-source build and the resource file is in a
        // subdirectory
        // of the .csproj file, we have to use relative pathnames, otherwise
        // visual studio does not show the file in the IDE. Sorry.
        if (obj.find(srcDir) == 0) {
          obj = this->ConvertPath(obj, true);
          ConvertToWindowsSlash(obj);
          useRelativePath = true;
        }
      }
      (*this->BuildFileStream) << obj << "\">\n";

      if (this->ProjectType != csproj) {
        this->WriteString("<DependentUpon>", 3);
        std::string hFileName = obj.substr(0, obj.find_last_of(".")) + ".h";
        (*this->BuildFileStream) << hFileName << "</DependentUpon>\n";

        for (std::string const& i : this->Configurations) {
          this->WritePlatformConfigTag("LogicalName", i, 3);
          if (this->GeneratorTarget->GetProperty("VS_GLOBAL_ROOTNAMESPACE") ||
              // Handle variant of VS_GLOBAL_<variable> for RootNamespace.
              this->GeneratorTarget->GetProperty("VS_GLOBAL_RootNamespace")) {
            (*this->BuildFileStream) << "$(RootNamespace).";
          }
          (*this->BuildFileStream) << "%(Filename)";
          (*this->BuildFileStream) << ".resources";
          (*this->BuildFileStream) << "</LogicalName>\n";
        }
      } else {
        std::string binDir = this->Makefile->GetCurrentBinaryDirectory();
        ConvertToWindowsSlash(binDir);
        // If the resource was NOT added using a relative path (which should
        // be the default), we have to provide a link here
        if (!useRelativePath) {
          std::string link;
          if (obj.find(srcDir) == 0) {
            link = obj.substr(srcDir.length() + 1);
          } else if (obj.find(binDir) == 0) {
            link = obj.substr(binDir.length() + 1);
          } else {
            link = cmsys::SystemTools::GetFilenameName(obj);
          }
          if (!link.empty()) {
            this->WriteString("<Link>", 3);
            (*this->BuildFileStream) << link << "</Link>\n";
          }
        }
        // Determine if this is a generated resource from a .Designer.cs file
        std::string designerResource =
          cmSystemTools::GetFilenamePath(oi->GetFullPath()) + "/" +
          cmSystemTools::GetFilenameWithoutLastExtension(oi->GetFullPath()) +
          ".Designer.cs";
        if (cmsys::SystemTools::FileExists(designerResource)) {
          std::string generator = "PublicResXFileCodeGenerator";
          if (const char* g = oi->GetProperty("VS_RESOURCE_GENERATOR")) {
            generator = g;
          }
          if (!generator.empty()) {
            this->WriteString("<Generator>", 3);
            (*this->BuildFileStream) << cmVS10EscapeXML(generator)
                                     << "</Generator>\n";
            if (designerResource.find(srcDir) == 0) {
              designerResource = designerResource.substr(srcDir.length() + 1);
            } else if (designerResource.find(binDir) == 0) {
              designerResource = designerResource.substr(binDir.length() + 1);
            } else {
              designerResource =
                cmsys::SystemTools::GetFilenameName(designerResource);
            }
            ConvertToWindowsSlash(designerResource);
            this->WriteString("<LastGenOutput>", 3);
            (*this->BuildFileStream) << designerResource
                                     << "</LastGenOutput>\n";
          }
        }
        const cmPropertyMap& props = oi->GetProperties();
        for (const auto& p : props) {
          static const std::string propNamePrefix = "VS_CSHARP_";
          if (p.first.find(propNamePrefix) == 0) {
            std::string tagName = p.first.substr(propNamePrefix.length());
            if (!tagName.empty()) {
              std::string value = props.GetPropertyValue(p.first);
              if (!value.empty()) {
                this->WriteString("<", 3);
                (*this->BuildFileStream) << tagName << ">";
                (*this->BuildFileStream) << cmVS10EscapeXML(value);
                (*this->BuildFileStream) << "</" << tagName << ">\n";
              }
            }
          }
        }
      }

      this->WriteString("</EmbeddedResource>\n", 2);
    }
    this->WriteString("</ItemGroup>\n", 1);
  }
}

void cmVisualStudio10TargetGenerator::WriteXamlFilesGroup()
{
  std::vector<cmSourceFile const*> xamlObjs;
  this->GeneratorTarget->GetXamlSources(xamlObjs, "");
  if (!xamlObjs.empty()) {
    this->WriteString("<ItemGroup>\n", 1);
    for (cmSourceFile const* oi : xamlObjs) {
      std::string obj = oi->GetFullPath();
      std::string xamlType;
      const char* xamlTypeProperty = oi->GetProperty("VS_XAML_TYPE");
      if (xamlTypeProperty) {
        xamlType = xamlTypeProperty;
      } else {
        xamlType = "Page";
      }

      this->WriteSource(xamlType, oi, ">\n");
      if (this->ProjectType == csproj && !this->InSourceBuild) {
        // add <Link> tag to written XAML source if necessary
        const std::string srcDir = this->Makefile->GetCurrentSourceDirectory();
        const std::string binDir = this->Makefile->GetCurrentBinaryDirectory();
        std::string link;
        if (obj.find(srcDir) == 0) {
          link = obj.substr(srcDir.length() + 1);
        } else if (obj.find(binDir) == 0) {
          link = obj.substr(binDir.length() + 1);
        } else {
          link = cmsys::SystemTools::GetFilenameName(obj);
        }
        if (!link.empty()) {
          ConvertToWindowsSlash(link);
          this->WriteString("<Link>", 3);
          (*this->BuildFileStream) << link << "</Link>\n";
        }
      }
      this->WriteString("<SubType>Designer</SubType>\n", 3);
      this->WriteString("</", 2);
      (*this->BuildFileStream) << xamlType << ">\n";
    }
    this->WriteString("</ItemGroup>\n", 1);
  }
}

void cmVisualStudio10TargetGenerator::WriteTargetSpecificReferences()
{
  if (this->MSTools) {
    if (this->GlobalGenerator->TargetsWindowsPhone() &&
        this->GlobalGenerator->GetSystemVersion() == "8.0") {
      this->WriteString("<Import Project=\""
                        "$(MSBuildExtensionsPath)\\Microsoft\\WindowsPhone\\v"
                        "$(TargetPlatformVersion)\\Microsoft.Cpp.WindowsPhone."
                        "$(TargetPlatformVersion).targets\" />\n",
                        1);
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteTargetsFileReferences()
{
  for (TargetsFileAndConfigs const& tac : this->TargetsFileAndConfigsVec) {
    this->WriteString("<Import Project=\"", 3);
    (*this->BuildFileStream) << tac.File << "\" ";
    (*this->BuildFileStream) << "Condition=\"";
    (*this->BuildFileStream) << "Exists('" << tac.File << "')";
    if (!tac.Configs.empty()) {
      (*this->BuildFileStream) << " And (";
      for (size_t j = 0; j < tac.Configs.size(); ++j) {
        if (j > 0) {
          (*this->BuildFileStream) << " Or ";
        }
        (*this->BuildFileStream) << "'$(Configuration)'=='" << tac.Configs[j]
                                 << "'";
      }
      (*this->BuildFileStream) << ")";
    }
    (*this->BuildFileStream) << "\" />\n";
  }
}

void cmVisualStudio10TargetGenerator::WriteWinRTReferences()
{
  std::vector<std::string> references;
  if (const char* vsWinRTReferences =
        this->GeneratorTarget->GetProperty("VS_WINRT_REFERENCES")) {
    cmSystemTools::ExpandListArgument(vsWinRTReferences, references);
  }

  if (this->GlobalGenerator->TargetsWindowsPhone() &&
      this->GlobalGenerator->GetSystemVersion() == "8.0" &&
      references.empty()) {
    references.push_back("platform.winmd");
  }
  if (!references.empty()) {
    this->WriteString("<ItemGroup>\n", 1);
    for (std::string const& ri : references) {
      this->WriteString("<Reference Include=\"", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(ri) << "\">\n";
      this->WriteString("<IsWinMDFile>true</IsWinMDFile>\n", 3);
      this->WriteString("</Reference>\n", 2);
    }
    this->WriteString("</ItemGroup>\n", 1);
  }
}

// ConfigurationType Application, Utility StaticLibrary DynamicLibrary

void cmVisualStudio10TargetGenerator::WriteProjectConfigurations()
{
  this->WriteString("<ItemGroup Label=\"ProjectConfigurations\">\n", 1);
  for (std::string const& c : this->Configurations) {
    this->WriteString("<ProjectConfiguration Include=\"", 2);
    (*this->BuildFileStream) << c << "|" << this->Platform << "\">\n";
    this->WriteString("<Configuration>", 3);
    (*this->BuildFileStream) << c << "</Configuration>\n";
    this->WriteString("<Platform>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(this->Platform)
                             << "</Platform>\n";
    this->WriteString("</ProjectConfiguration>\n", 2);
  }
  this->WriteString("</ItemGroup>\n", 1);
}

void cmVisualStudio10TargetGenerator::WriteProjectConfigurationValues()
{
  for (std::string const& c : this->Configurations) {
    this->WritePlatformConfigTag("PropertyGroup", c, 1,
                                 " Label=\"Configuration\"", "\n");

    if (this->ProjectType != csproj) {
      std::string configType = "<ConfigurationType>";
      if (const char* vsConfigurationType =
            this->GeneratorTarget->GetProperty("VS_CONFIGURATION_TYPE")) {
        configType += cmVS10EscapeXML(vsConfigurationType);
      } else {
        switch (this->GeneratorTarget->GetType()) {
          case cmStateEnums::SHARED_LIBRARY:
          case cmStateEnums::MODULE_LIBRARY:
            configType += "DynamicLibrary";
            break;
          case cmStateEnums::OBJECT_LIBRARY:
          case cmStateEnums::STATIC_LIBRARY:
            configType += "StaticLibrary";
            break;
          case cmStateEnums::EXECUTABLE:
            if (this->NsightTegra &&
                !this->GeneratorTarget->GetPropertyAsBool("ANDROID_GUI")) {
              // Android executables are .so too.
              configType += "DynamicLibrary";
            } else {
              configType += "Application";
            }
            break;
          case cmStateEnums::UTILITY:
          case cmStateEnums::GLOBAL_TARGET:
            if (this->NsightTegra) {
              // Tegra-Android platform does not understand "Utility".
              configType += "StaticLibrary";
            } else {
              configType += "Utility";
            }
            break;
          case cmStateEnums::UNKNOWN_LIBRARY:
          case cmStateEnums::INTERFACE_LIBRARY:
            break;
        }
      }
      configType += "</ConfigurationType>\n";
      this->WriteString(configType.c_str(), 2);
    }

    if (this->MSTools) {
      if (!this->Managed) {
        this->WriteMSToolConfigurationValues(c);
      } else {
        this->WriteMSToolConfigurationValuesManaged(c);
      }
    } else if (this->NsightTegra) {
      this->WriteNsightTegraConfigurationValues(c);
    }

    this->WriteString("</PropertyGroup>\n", 1);
  }
}

void cmVisualStudio10TargetGenerator::WriteMSToolConfigurationValues(
  std::string const& config)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  const char* mfcFlag = this->Makefile->GetDefinition("CMAKE_MFC_FLAG");
  if (mfcFlag) {
    std::string const mfcFlagValue = mfcFlag;

    std::string useOfMfcValue = "false";
    if (this->GeneratorTarget->GetType() <= cmStateEnums::OBJECT_LIBRARY) {
      if (mfcFlagValue == "1") {
        useOfMfcValue = "Static";
      } else if (mfcFlagValue == "2") {
        useOfMfcValue = "Dynamic";
      }
    }
    std::string mfcLine = "<UseOfMfc>";
    mfcLine += useOfMfcValue + "</UseOfMfc>\n";
    this->WriteString(mfcLine.c_str(), 2);
  }

  if ((this->GeneratorTarget->GetType() <= cmStateEnums::OBJECT_LIBRARY &&
       this->ClOptions[config]->UsingUnicode()) ||
      this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_COMPONENT") ||
      this->GlobalGenerator->TargetsWindowsPhone() ||
      this->GlobalGenerator->TargetsWindowsStore() ||
      this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_EXTENSIONS")) {
    this->WriteString("<CharacterSet>Unicode</CharacterSet>\n", 2);
  } else if (this->GeneratorTarget->GetType() <=
               cmStateEnums::MODULE_LIBRARY &&
             this->ClOptions[config]->UsingSBCS()) {
    this->WriteString("<CharacterSet>NotSet</CharacterSet>\n", 2);
  } else {
    this->WriteString("<CharacterSet>MultiByte</CharacterSet>\n", 2);
  }
  if (const char* toolset = gg->GetPlatformToolset()) {
    std::string pts = "<PlatformToolset>";
    pts += toolset;
    pts += "</PlatformToolset>\n";
    this->WriteString(pts.c_str(), 2);
  }
  if (this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_COMPONENT") ||
      this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_EXTENSIONS")) {
    this->WriteString("<WindowsAppContainer>true"
                      "</WindowsAppContainer>\n",
                      2);
  }
}

void cmVisualStudio10TargetGenerator::WriteMSToolConfigurationValuesManaged(
  std::string const& config)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;

  Options& o = *(this->ClOptions[config]);

  if (o.IsDebug()) {
    this->WriteString("<DebugSymbols>true</DebugSymbols>\n", 2);
    this->WriteString("<DefineDebug>true</DefineDebug>\n", 2);
  }

  std::string outDir = this->GeneratorTarget->GetDirectory(config) + "/";
  ConvertToWindowsSlash(outDir);
  this->WriteString("<OutputPath>", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(outDir) << "</OutputPath>\n";

  if (o.HasFlag("Platform")) {
    this->WriteString("<PlatformTarget>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(o.GetFlag("Platform"))
                             << "</PlatformTarget>\n";
    o.RemoveFlag("Platform");
  }

  if (const char* toolset = gg->GetPlatformToolset()) {
    this->WriteString("<PlatformToolset>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(toolset)
                             << "</PlatformToolset>\n";
  }

  std::string postfixName = cmSystemTools::UpperCase(config);
  postfixName += "_POSTFIX";
  std::string assemblyName = this->GeneratorTarget->GetOutputName(
    config, cmStateEnums::RuntimeBinaryArtifact);
  if (const char* postfix = this->GeneratorTarget->GetProperty(postfixName)) {
    assemblyName += postfix;
  }
  this->WriteString("<AssemblyName>", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(assemblyName)
                           << "</AssemblyName>\n";

  if (cmStateEnums::EXECUTABLE == this->GeneratorTarget->GetType()) {
    this->WriteString("<StartAction>Program</StartAction>\n", 2);
    this->WriteString("<StartProgram>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(outDir)
                             << cmVS10EscapeXML(assemblyName)
                             << ".exe</StartProgram>\n";
  }

  o.OutputFlagMap(*this->BuildFileStream, "    ");
}

//----------------------------------------------------------------------------
void cmVisualStudio10TargetGenerator::WriteNsightTegraConfigurationValues(
  std::string const&)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  const char* toolset = gg->GetPlatformToolset();
  std::string ntv = "<NdkToolchainVersion>";
  ntv += toolset ? toolset : "Default";
  ntv += "</NdkToolchainVersion>\n";
  this->WriteString(ntv.c_str(), 2);
  if (const char* minApi =
        this->GeneratorTarget->GetProperty("ANDROID_API_MIN")) {
    this->WriteString("<AndroidMinAPI>", 2);
    (*this->BuildFileStream) << "android-" << cmVS10EscapeXML(minApi)
                             << "</AndroidMinAPI>\n";
  }
  if (const char* api = this->GeneratorTarget->GetProperty("ANDROID_API")) {
    this->WriteString("<AndroidTargetAPI>", 2);
    (*this->BuildFileStream) << "android-" << cmVS10EscapeXML(api)
                             << "</AndroidTargetAPI>\n";
  }

  if (const char* cpuArch =
        this->GeneratorTarget->GetProperty("ANDROID_ARCH")) {
    this->WriteString("<AndroidArch>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(cpuArch) << "</AndroidArch>\n";
  }

  if (const char* stlType =
        this->GeneratorTarget->GetProperty("ANDROID_STL_TYPE")) {
    this->WriteString("<AndroidStlType>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(stlType)
                             << "</AndroidStlType>\n";
  }
}

void cmVisualStudio10TargetGenerator::WriteCustomCommands()
{
  this->SourcesVisited.clear();
  this->CSharpCustomCommandNames.clear();
  std::vector<cmSourceFile const*> customCommands;
  this->GeneratorTarget->GetCustomCommands(customCommands, "");
  for (cmSourceFile const* si : customCommands) {
    this->WriteCustomCommand(si);
  }

  // Add CMakeLists.txt file with rule to re-run CMake for user convenience.
  if (this->GeneratorTarget->GetType() != cmStateEnums::GLOBAL_TARGET &&
      this->GeneratorTarget->GetName() != CMAKE_CHECK_BUILD_SYSTEM_TARGET) {
    if (cmSourceFile const* sf =
          this->LocalGenerator->CreateVCProjBuildRule()) {
      this->WriteCustomCommand(sf);
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteCustomCommand(
  cmSourceFile const* sf)
{
  if (this->SourcesVisited.insert(sf).second) {
    if (std::vector<cmSourceFile*> const* depends =
          this->GeneratorTarget->GetSourceDepends(sf)) {
      for (cmSourceFile const* di : *depends) {
        this->WriteCustomCommand(di);
      }
    }
    if (cmCustomCommand const* command = sf->GetCustomCommand()) {
      // C# projects write their <Target> within WriteCustomRule()
      if (this->ProjectType != csproj) {
        this->WriteString("<ItemGroup>\n", 1);
      }
      this->WriteCustomRule(sf, *command);
      if (this->ProjectType != csproj) {
        this->WriteString("</ItemGroup>\n", 1);
      }
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteCustomRule(
  cmSourceFile const* source, cmCustomCommand const& command)
{
  std::string sourcePath = source->GetFullPath();
  // VS 10 will always rebuild a custom command attached to a .rule
  // file that doesn't exist so create the file explicitly.
  if (source->GetPropertyAsBool("__CMAKE_RULE")) {
    if (!cmSystemTools::FileExists(sourcePath)) {
      // Make sure the path exists for the file
      std::string path = cmSystemTools::GetFilenamePath(sourcePath);
      cmSystemTools::MakeDirectory(path);
      cmsys::ofstream fout(sourcePath.c_str());
      if (fout) {
        fout << "# generated from CMake\n";
        fout.flush();
        fout.close();
        // Force given file to have a very old timestamp, thus
        // preventing dependent rebuilds.
        this->ForceOld(sourcePath);
      } else {
        std::string error = "Could not create file: [";
        error += sourcePath;
        error += "]  ";
        cmSystemTools::Error(error.c_str(),
                             cmSystemTools::GetLastSystemError().c_str());
      }
    }
  }
  cmLocalVisualStudio7Generator* lg = this->LocalGenerator;

  if (this->ProjectType != csproj) {
    this->WriteSource("CustomBuild", source, ">\n");
  } else {
    this->WriteString("<ItemGroup>\n", 1);
    std::string link;
    this->GetCSharpSourceLink(source, link);
    this->WriteSource("None", source, ">\n");
    if (!link.empty()) {
      this->WriteString("<Link>", 3);
      (*this->BuildFileStream) << link << "</Link>\n";
    }
    this->WriteString("</None>\n", 2);
    this->WriteString("</ItemGroup>\n", 1);
  }
  for (std::string const& c : this->Configurations) {
    cmCustomCommandGenerator ccg(command, c, lg);
    std::string comment = lg->ConstructComment(ccg);
    comment = cmVS10EscapeComment(comment);
    std::string script = cmVS10EscapeXML(lg->ConstructScript(ccg));
    // input files for custom command
    std::stringstream inputs;
    inputs << cmVS10EscapeXML(source->GetFullPath());
    for (std::string const& d : ccg.GetDepends()) {
      std::string dep;
      if (lg->GetRealDependency(d, c, dep)) {
        ConvertToWindowsSlash(dep);
        inputs << ";" << cmVS10EscapeXML(dep);
      }
    }
    // output files for custom command
    std::stringstream outputs;
    const char* sep = "";
    for (std::string const& o : ccg.GetOutputs()) {
      std::string out = o;
      ConvertToWindowsSlash(out);
      outputs << sep << cmVS10EscapeXML(out);
      sep = ";";
    }
    if (this->ProjectType == csproj) {
      std::string name = "CustomCommand_" + c + "_" +
        cmSystemTools::ComputeStringMD5(sourcePath);
      std::string inputs_s = inputs.str();
      std::string outputs_s = outputs.str();
      comment = cmVS10EscapeQuotes(comment);
      script = cmVS10EscapeQuotes(script);
      inputs_s = cmVS10EscapeQuotes(inputs_s);
      outputs_s = cmVS10EscapeQuotes(outputs_s);
      this->WriteCustomRuleCSharp(c, name, script, inputs_s, outputs_s,
                                  comment);
    } else {
      this->WriteCustomRuleCpp(c, script, inputs.str(), outputs.str(),
                               comment);
    }
  }
  if (this->ProjectType != csproj) {
    this->WriteString("</CustomBuild>\n", 2);
  }
}

void cmVisualStudio10TargetGenerator::WriteCustomRuleCpp(
  std::string const& config, std::string const& script,
  std::string const& inputs, std::string const& outputs,
  std::string const& comment)
{
  this->WritePlatformConfigTag("Message", config, 3);
  (*this->BuildFileStream) << cmVS10EscapeXML(comment) << "</Message>\n";
  this->WritePlatformConfigTag("Command", config, 3);
  (*this->BuildFileStream) << script << "</Command>\n";
  this->WritePlatformConfigTag("AdditionalInputs", config, 3);
  (*this->BuildFileStream) << inputs;
  (*this->BuildFileStream) << ";%(AdditionalInputs)</AdditionalInputs>\n";
  this->WritePlatformConfigTag("Outputs", config, 3);
  (*this->BuildFileStream) << outputs << "</Outputs>\n";
  if (this->LocalGenerator->GetVersion() >
      cmGlobalVisualStudioGenerator::VS10) {
    // VS >= 11 let us turn off linking of custom command outputs.
    this->WritePlatformConfigTag("LinkObjects", config, 3);
    (*this->BuildFileStream) << "false</LinkObjects>\n";
  }
}

void cmVisualStudio10TargetGenerator::WriteCustomRuleCSharp(
  std::string const& config, std::string const& name,
  std::string const& script, std::string const& inputs,
  std::string const& outputs, std::string const& comment)
{
  this->CSharpCustomCommandNames.insert(name);
  std::stringstream attributes;
  attributes << "\n    Name=\"" << name << "\"";
  attributes << "\n    Inputs=\"" << inputs << "\"";
  attributes << "\n    Outputs=\"" << outputs << "\"";
  this->WritePlatformConfigTag("Target", config, 1, attributes.str().c_str(),
                               "\n");
  if (!comment.empty()) {
    this->WriteString("<Exec Command=\"", 2);
    (*this->BuildFileStream) << "echo " << cmVS10EscapeXML(comment)
                             << "\" />\n";
  }
  this->WriteString("<Exec Command=\"", 2);
  (*this->BuildFileStream) << script << "\" />\n";
  this->WriteString("</Target>\n", 1);
}

std::string cmVisualStudio10TargetGenerator::ConvertPath(
  std::string const& path, bool forceRelative)
{
  return forceRelative
    ? cmSystemTools::RelativePath(
        this->LocalGenerator->GetCurrentBinaryDirectory(), path)
    : path;
}

static void ConvertToWindowsSlash(std::string& s)
{
  // first convert all of the slashes
  std::string::size_type pos = 0;
  while ((pos = s.find('/', pos)) != std::string::npos) {
    s[pos] = '\\';
    pos++;
  }
}

void cmVisualStudio10TargetGenerator::WriteGroups()
{
  if (this->ProjectType == csproj) {
    return;
  }

  // collect up group information
  std::vector<cmSourceGroup> sourceGroups = this->Makefile->GetSourceGroups();

  std::vector<cmGeneratorTarget::AllConfigSource> const& sources =
    this->GeneratorTarget->GetAllConfigSources();

  std::set<cmSourceGroup*> groupsUsed;
  for (cmGeneratorTarget::AllConfigSource const& si : sources) {
    std::string const& source = si.Source->GetFullPath();
    cmSourceGroup* sourceGroup =
      this->Makefile->FindSourceGroup(source, sourceGroups);
    groupsUsed.insert(sourceGroup);
  }

  this->AddMissingSourceGroups(groupsUsed, sourceGroups);

  // Write out group file
  std::string path = this->LocalGenerator->GetCurrentBinaryDirectory();
  path += "/";
  path += this->Name;
  path += computeProjectFileExtension(this->GeneratorTarget,
                                      *this->Configurations.begin());
  path += ".filters";
  cmGeneratedFileStream fout(path.c_str());
  fout.SetCopyIfDifferent(true);
  char magic[] = { char(0xEF), char(0xBB), char(0xBF) };
  fout.write(magic, 3);
  cmGeneratedFileStream* save = this->BuildFileStream;
  this->BuildFileStream = &fout;

  // get the tools version to use
  const std::string toolsVer(this->GlobalGenerator->GetToolsVersion());
  std::string project_defaults = "<?xml version=\"1.0\" encoding=\"" +
    this->GlobalGenerator->Encoding() + "\"?>\n";
  project_defaults.append("<Project ToolsVersion=\"");
  project_defaults.append(toolsVer + "\" ");
  project_defaults.append(
    "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n");
  this->WriteString(project_defaults.c_str(), 0);

  for (auto const& ti : this->Tools) {
    this->WriteGroupSources(ti.first.c_str(), ti.second, sourceGroups);
  }

  // Added files are images and the manifest.
  if (!this->AddedFiles.empty()) {
    this->WriteString("<ItemGroup>\n", 1);
    for (std::string const& oi : this->AddedFiles) {
      std::string fileName =
        cmSystemTools::LowerCase(cmSystemTools::GetFilenameName(oi));
      if (fileName == "wmappmanifest.xml") {
        this->WriteString("<XML Include=\"", 2);
        (*this->BuildFileStream) << oi << "\">\n";
        this->WriteString("<Filter>Resource Files</Filter>\n", 3);
        this->WriteString("</XML>\n", 2);
      } else if (cmSystemTools::GetFilenameExtension(fileName) ==
                 ".appxmanifest") {
        this->WriteString("<AppxManifest Include=\"", 2);
        (*this->BuildFileStream) << oi << "\">\n";
        this->WriteString("<Filter>Resource Files</Filter>\n", 3);
        this->WriteString("</AppxManifest>\n", 2);
      } else if (cmSystemTools::GetFilenameExtension(fileName) == ".pfx") {
        this->WriteString("<None Include=\"", 2);
        (*this->BuildFileStream) << oi << "\">\n";
        this->WriteString("<Filter>Resource Files</Filter>\n", 3);
        this->WriteString("</None>\n", 2);
      } else {
        this->WriteString("<Image Include=\"", 2);
        (*this->BuildFileStream) << oi << "\">\n";
        this->WriteString("<Filter>Resource Files</Filter>\n", 3);
        this->WriteString("</Image>\n", 2);
      }
    }
    this->WriteString("</ItemGroup>\n", 1);
  }

  std::vector<cmSourceFile const*> resxObjs;
  this->GeneratorTarget->GetResxSources(resxObjs, "");
  if (!resxObjs.empty()) {
    this->WriteString("<ItemGroup>\n", 1);
    for (cmSourceFile const* oi : resxObjs) {
      std::string obj = oi->GetFullPath();
      this->WriteString("<EmbeddedResource Include=\"", 2);
      ConvertToWindowsSlash(obj);
      (*this->BuildFileStream) << cmVS10EscapeXML(obj) << "\">\n";
      this->WriteString("<Filter>Resource Files</Filter>\n", 3);
      this->WriteString("</EmbeddedResource>\n", 2);
    }
    this->WriteString("</ItemGroup>\n", 1);
  }

  this->WriteString("<ItemGroup>\n", 1);
  std::vector<cmSourceGroup*> groupsVec(groupsUsed.begin(), groupsUsed.end());
  std::sort(groupsVec.begin(), groupsVec.end(),
            [](cmSourceGroup* l, cmSourceGroup* r) {
              return l->GetFullName() < r->GetFullName();
            });
  for (cmSourceGroup* sg : groupsVec) {
    std::string const& name = sg->GetFullName();
    if (!name.empty()) {
      this->WriteString("<Filter Include=\"", 2);
      (*this->BuildFileStream) << name << "\">\n";
      std::string guidName = "SG_Filter_";
      guidName += name;
      this->WriteString("<UniqueIdentifier>", 3);
      std::string guid = this->GlobalGenerator->GetGUID(guidName);
      (*this->BuildFileStream) << "{" << guid << "}"
                               << "</UniqueIdentifier>\n";
      this->WriteString("</Filter>\n", 2);
    }
  }

  if (!resxObjs.empty() || !this->AddedFiles.empty()) {
    this->WriteString("<Filter Include=\"Resource Files\">\n", 2);
    std::string guidName = "SG_Filter_Resource Files";
    this->WriteString("<UniqueIdentifier>", 3);
    std::string guid = this->GlobalGenerator->GetGUID(guidName);
    (*this->BuildFileStream) << "{" << guid << "}"
                             << "</UniqueIdentifier>\n";
    this->WriteString("<Extensions>rc;ico;cur;bmp;dlg;rc2;rct;bin;rgs;", 3);
    (*this->BuildFileStream) << "gif;jpg;jpeg;jpe;resx;tiff;tif;png;wav;";
    (*this->BuildFileStream) << "mfcribbon-ms</Extensions>\n";
    this->WriteString("</Filter>\n", 2);
  }

  this->WriteString("</ItemGroup>\n", 1);
  this->WriteString("</Project>\n", 0);
  // restore stream pointer
  this->BuildFileStream = save;

  if (fout.Close()) {
    this->GlobalGenerator->FileReplacedDuringGenerate(path);
  }
}

// Add to groupsUsed empty source groups that have non-empty children.
void cmVisualStudio10TargetGenerator::AddMissingSourceGroups(
  std::set<cmSourceGroup*>& groupsUsed,
  const std::vector<cmSourceGroup>& allGroups)
{
  for (cmSourceGroup const& current : allGroups) {
    std::vector<cmSourceGroup> const& children = current.GetGroupChildren();
    if (children.empty()) {
      continue; // the group is really empty
    }

    this->AddMissingSourceGroups(groupsUsed, children);

    cmSourceGroup* current_ptr = const_cast<cmSourceGroup*>(&current);
    if (groupsUsed.find(current_ptr) != groupsUsed.end()) {
      continue; // group has already been added to set
    }

    // check if it least one of the group's descendants is not empty
    // (at least one child must already have been added)
    std::vector<cmSourceGroup>::const_iterator child_it = children.begin();
    while (child_it != children.end()) {
      cmSourceGroup* child_ptr = const_cast<cmSourceGroup*>(&(*child_it));
      if (groupsUsed.find(child_ptr) != groupsUsed.end()) {
        break; // found a child that was already added => add current group too
      }
      child_it++;
    }

    if (child_it == children.end()) {
      continue; // no descendants have source files => ignore this group
    }

    groupsUsed.insert(current_ptr);
  }
}

void cmVisualStudio10TargetGenerator::WriteGroupSources(
  std::string const& name, ToolSources const& sources,
  std::vector<cmSourceGroup>& sourceGroups)
{
  this->WriteString("<ItemGroup>\n", 1);
  for (ToolSource const& s : sources) {
    cmSourceFile const* sf = s.SourceFile;
    std::string const& source = sf->GetFullPath();
    cmSourceGroup* sourceGroup =
      this->Makefile->FindSourceGroup(source, sourceGroups);
    std::string const& filter = sourceGroup->GetFullName();
    this->WriteString("<", 2);
    std::string path = this->ConvertPath(source, s.RelativePath);
    ConvertToWindowsSlash(path);
    (*this->BuildFileStream) << name << " Include=\"" << cmVS10EscapeXML(path);
    if (!filter.empty()) {
      (*this->BuildFileStream) << "\">\n";
      this->WriteString("<Filter>", 3);
      (*this->BuildFileStream) << filter << "</Filter>\n";
      this->WriteString("</", 2);
      (*this->BuildFileStream) << name << ">\n";
    } else {
      (*this->BuildFileStream) << "\" />\n";
    }
  }
  this->WriteString("</ItemGroup>\n", 1);
}

void cmVisualStudio10TargetGenerator::WriteHeaderSource(cmSourceFile const* sf)
{
  std::string const& fileName = sf->GetFullPath();
  if (this->IsResxHeader(fileName)) {
    this->WriteSource("ClInclude", sf, ">\n");
    this->WriteString("<FileType>CppForm</FileType>\n", 3);
    this->WriteString("</ClInclude>\n", 2);
  } else if (this->IsXamlHeader(fileName)) {
    this->WriteSource("ClInclude", sf, ">\n");
    this->WriteString("<DependentUpon>", 3);
    std::string xamlFileName = fileName.substr(0, fileName.find_last_of("."));
    (*this->BuildFileStream) << xamlFileName << "</DependentUpon>\n";
    this->WriteString("</ClInclude>\n", 2);
  } else {
    this->WriteSource("ClInclude", sf);
  }
}

void cmVisualStudio10TargetGenerator::WriteExtraSource(cmSourceFile const* sf)
{
  bool toolHasSettings = false;
  std::string tool = "None";
  std::string shaderType;
  std::string shaderEntryPoint;
  std::string shaderModel;
  std::string shaderAdditionalFlags;
  std::string shaderDisableOptimizations;
  std::string shaderEnableDebug;
  std::string outputHeaderFile;
  std::string variableName;
  std::string settingsGenerator;
  std::string settingsLastGenOutput;
  std::string sourceLink;
  std::string subType;
  std::string copyToOutDir;
  std::string includeInVsix;
  std::string ext = cmSystemTools::LowerCase(sf->GetExtension());
  if (this->ProjectType == csproj) {
    // EVERY extra source file must have a <Link>, otherwise it might not
    // be visible in Visual Studio at all. The path relative to current
    // source- or binary-dir is used within the link, if the file is
    // in none of these paths, it is added with the plain filename without
    // any path. This means the file will show up at root-level of the csproj
    // (where CMakeLists.txt etc. are).
    if (!this->InSourceBuild) {
      toolHasSettings = true;
      std::string fullFileName = sf->GetFullPath();
      std::string srcDir = this->Makefile->GetCurrentSourceDirectory();
      std::string binDir = this->Makefile->GetCurrentBinaryDirectory();
      if (fullFileName.find(binDir) != std::string::npos) {
        sourceLink.clear();
      } else if (fullFileName.find(srcDir) != std::string::npos) {
        sourceLink = fullFileName.substr(srcDir.length() + 1);
      } else {
        // fallback: add plain filename without any path
        sourceLink = cmsys::SystemTools::GetFilenameName(fullFileName);
      }
      if (!sourceLink.empty()) {
        ConvertToWindowsSlash(sourceLink);
      }
    }
  }
  if (ext == "hlsl") {
    tool = "FXCompile";
    // Figure out the type of shader compiler to use.
    if (const char* st = sf->GetProperty("VS_SHADER_TYPE")) {
      shaderType = st;
      toolHasSettings = true;
    }
    // Figure out which entry point to use if any
    if (const char* se = sf->GetProperty("VS_SHADER_ENTRYPOINT")) {
      shaderEntryPoint = se;
      toolHasSettings = true;
    }
    // Figure out which shader model to use if any
    if (const char* sm = sf->GetProperty("VS_SHADER_MODEL")) {
      shaderModel = sm;
      toolHasSettings = true;
    }
    // Figure out which output header file to use if any
    if (const char* ohf = sf->GetProperty("VS_SHADER_OUTPUT_HEADER_FILE")) {
      outputHeaderFile = ohf;
      toolHasSettings = true;
    }
    // Figure out which variable name to use if any
    if (const char* vn = sf->GetProperty("VS_SHADER_VARIABLE_NAME")) {
      variableName = vn;
      toolHasSettings = true;
    }
    // Figure out if there's any additional flags to use
    if (const char* saf = sf->GetProperty("VS_SHADER_FLAGS")) {
      shaderAdditionalFlags = saf;
      toolHasSettings = true;
    }
    // Figure out if debug information should be generated
    if (const char* sed = sf->GetProperty("VS_SHADER_ENABLE_DEBUG")) {
      shaderEnableDebug = cmSystemTools::IsOn(sed) ? "true" : "false";
      toolHasSettings = true;
    }
    // Figure out if optimizations should be disabled
    if (const char* sdo = sf->GetProperty("VS_SHADER_DISABLE_OPTIMIZATIONS")) {
      shaderDisableOptimizations = cmSystemTools::IsOn(sdo) ? "true" : "false";
      toolHasSettings = true;
    }
  } else if (ext == "jpg" || ext == "png") {
    tool = "Image";
  } else if (ext == "resw") {
    tool = "PRIResource";
  } else if (ext == "xml") {
    tool = "XML";
  } else if (ext == "natvis") {
    tool = "Natvis";
  } else if (ext == "settings") {
    settingsLastGenOutput =
      cmsys::SystemTools::GetFilenameName(sf->GetFullPath());
    std::size_t pos = settingsLastGenOutput.find(".settings");
    settingsLastGenOutput.replace(pos, 9, ".Designer.cs");
    settingsGenerator = "SettingsSingleFileGenerator";
    toolHasSettings = true;
  } else if (ext == "vsixmanifest") {
    subType = "Designer";
  }
  if (const char* c = sf->GetProperty("VS_COPY_TO_OUT_DIR")) {
    copyToOutDir = c;
    toolHasSettings = true;
  }
  if (sf->GetPropertyAsBool("VS_INCLUDE_IN_VSIX")) {
    includeInVsix = "True";
    tool = "Content";
    toolHasSettings = true;
  }

  // Collect VS_CSHARP_* property values (if some are set)
  std::map<std::string, std::string> sourceFileTags;
  this->GetCSharpSourceProperties(sf, sourceFileTags);

  if (this->NsightTegra) {
    // Nsight Tegra needs specific file types to check up-to-dateness.
    std::string name = cmSystemTools::LowerCase(sf->GetLocation().GetName());
    if (name == "androidmanifest.xml" || name == "build.xml" ||
        name == "proguard.cfg" || name == "proguard-project.txt" ||
        ext == "properties") {
      tool = "AndroidBuild";
    } else if (ext == "java") {
      tool = "JCompile";
    } else if (ext == "asm" || ext == "s") {
      tool = "ClCompile";
    }
  }

  const char* toolOverride = sf->GetProperty("VS_TOOL_OVERRIDE");
  if (toolOverride && *toolOverride) {
    tool = toolOverride;
  }

  std::string deployContent;
  std::string deployLocation;
  if (this->GlobalGenerator->TargetsWindowsPhone() ||
      this->GlobalGenerator->TargetsWindowsStore()) {
    const char* content = sf->GetProperty("VS_DEPLOYMENT_CONTENT");
    if (content && *content) {
      toolHasSettings = true;
      deployContent = content;

      const char* location = sf->GetProperty("VS_DEPLOYMENT_LOCATION");
      if (location && *location) {
        deployLocation = location;
      }
    }
  }

  if (toolHasSettings) {
    this->WriteSource(tool, sf, ">\n");

    if (!deployContent.empty()) {
      cmGeneratorExpression ge;
      std::unique_ptr<cmCompiledGeneratorExpression> cge =
        ge.Parse(deployContent);
      // Deployment location cannot be set on a configuration basis
      if (!deployLocation.empty()) {
        this->WriteString("<Link>", 3);
        (*this->BuildFileStream) << deployLocation
                                 << "\\%(FileName)%(Extension)";
        this->WriteString("</Link>\n", 0);
      }
      for (size_t i = 0; i != this->Configurations.size(); ++i) {
        if (0 == strcmp(cge->Evaluate(this->LocalGenerator,
                                      this->Configurations[i]),
                        "1")) {
          this->WriteString("<DeploymentContent Condition=\""
                            "'$(Configuration)|$(Platform)'=='",
                            3);
          (*this->BuildFileStream) << this->Configurations[i] << "|"
                                   << this->Platform << "'\">true";
          this->WriteString("</DeploymentContent>\n", 0);
        } else {
          this->WriteString("<ExcludedFromBuild Condition=\""
                            "'$(Configuration)|$(Platform)'=='",
                            3);
          (*this->BuildFileStream) << this->Configurations[i] << "|"
                                   << this->Platform << "'\">true";
          this->WriteString("</ExcludedFromBuild>\n", 0);
        }
      }
    }
    if (!shaderType.empty()) {
      this->WriteString("<ShaderType>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(shaderType)
                               << "</ShaderType>\n";
    }
    if (!shaderEntryPoint.empty()) {
      this->WriteString("<EntryPointName>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(shaderEntryPoint)
                               << "</EntryPointName>\n";
    }
    if (!shaderModel.empty()) {
      this->WriteString("<ShaderModel>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(shaderModel)
                               << "</ShaderModel>\n";
    }
    if (!outputHeaderFile.empty()) {
      for (size_t i = 0; i != this->Configurations.size(); ++i) {
        this->WriteString("<HeaderFileOutput Condition=\""
                          "'$(Configuration)|$(Platform)'=='",
                          3);
        (*this->BuildFileStream) << this->Configurations[i] << "|"
                                 << this->Platform << "'\">"
                                 << cmVS10EscapeXML(outputHeaderFile);
        this->WriteString("</HeaderFileOutput>\n", 0);
      }
    }
    if (!variableName.empty()) {
      for (size_t i = 0; i != this->Configurations.size(); ++i) {
        this->WriteString("<VariableName Condition=\""
                          "'$(Configuration)|$(Platform)'=='",
                          3);
        (*this->BuildFileStream) << this->Configurations[i] << "|"
                                 << this->Platform << "'\">"
                                 << cmVS10EscapeXML(variableName);
        this->WriteString("</VariableName>\n", 0);
      }
    }
    if (!shaderEnableDebug.empty()) {
      this->WriteString("<EnableDebuggingInformation>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(shaderEnableDebug)
                               << "</EnableDebuggingInformation>\n";
    }
    if (!shaderDisableOptimizations.empty()) {
      this->WriteString("<DisableOptimizations>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(shaderDisableOptimizations)
                               << "</DisableOptimizations>\n";
    }
    if (!shaderAdditionalFlags.empty()) {
      this->WriteString("<AdditionalOptions>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(shaderAdditionalFlags)
                               << "</AdditionalOptions>\n";
    }
    if (!settingsGenerator.empty()) {
      this->WriteString("<Generator>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(settingsGenerator)
                               << "</Generator>\n";
    }
    if (!settingsLastGenOutput.empty()) {
      this->WriteString("<LastGenOutput>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(settingsLastGenOutput)
                               << "</LastGenOutput>\n";
    }
    if (!sourceLink.empty()) {
      this->WriteString("<Link>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(sourceLink) << "</Link>\n";
    }
    if (!subType.empty()) {
      this->WriteString("<SubType>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(subType) << "</SubType>\n";
    }
    if (!copyToOutDir.empty()) {
      this->WriteString("<CopyToOutputDirectory>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(copyToOutDir)
                               << "</CopyToOutputDirectory>\n";
    }
    if (!includeInVsix.empty()) {
      this->WriteString("<IncludeInVSIX>", 3);
      (*this->BuildFileStream) << cmVS10EscapeXML(includeInVsix)
                               << "</IncludeInVSIX>\n";
    }
    // write source file specific tags
    this->WriteCSharpSourceProperties(sourceFileTags);
    this->WriteString("</", 2);
    (*this->BuildFileStream) << tool << ">\n";
  } else {
    this->WriteSource(tool, sf);
  }
}

void cmVisualStudio10TargetGenerator::WriteSource(std::string const& tool,
                                                  cmSourceFile const* sf,
                                                  const char* end)
{
  // Visual Studio tools append relative paths to the current dir, as in:
  //
  //  c:\path\to\current\dir\..\..\..\relative\path\to\source.c
  //
  // and fail if this exceeds the maximum allowed path length.  Our path
  // conversion uses full paths when possible to allow deeper trees.
  // However, CUDA 8.0 msbuild rules fail on absolute paths so for CUDA
  // we must use relative paths.
  bool forceRelative = sf->GetLanguage() == "CUDA";
  std::string sourceFile = this->ConvertPath(sf->GetFullPath(), forceRelative);
  if (this->LocalGenerator->GetVersion() ==
        cmGlobalVisualStudioGenerator::VS10 &&
      cmSystemTools::FileIsFullPath(sourceFile)) {
    // Normal path conversion resulted in a full path.  VS 10 (but not 11)
    // refuses to show the property page in the IDE for a source file with a
    // full path (not starting in a '.' or '/' AFAICT).  CMake <= 2.8.4 used a
    // relative path but to allow deeper build trees CMake 2.8.[5678] used a
    // full path except for custom commands.  Custom commands do not work
    // without a relative path, but they do not seem to be involved in tools
    // with the above behavior.  For other sources we now use a relative path
    // when the combined path will not be too long so property pages appear.
    std::string sourceRel = this->ConvertPath(sf->GetFullPath(), true);
    size_t const maxLen = 250;
    if (sf->GetCustomCommand() ||
        ((strlen(this->LocalGenerator->GetCurrentBinaryDirectory()) + 1 +
          sourceRel.length()) <= maxLen)) {
      forceRelative = true;
      sourceFile = sourceRel;
    } else {
      this->GlobalGenerator->PathTooLong(this->GeneratorTarget, sf, sourceRel);
    }
  }
  ConvertToWindowsSlash(sourceFile);
  this->WriteString("<", 2);
  (*this->BuildFileStream) << tool << " Include=\""
                           << cmVS10EscapeXML(sourceFile) << "\""
                           << (end ? end : " />\n");

  ToolSource toolSource = { sf, forceRelative };
  this->Tools[tool].push_back(toolSource);
}

void cmVisualStudio10TargetGenerator::WriteAllSources()
{
  if (this->GeneratorTarget->GetType() > cmStateEnums::UTILITY) {
    return;
  }
  this->WriteString("<ItemGroup>\n", 1);

  std::vector<size_t> all_configs;
  for (size_t ci = 0; ci < this->Configurations.size(); ++ci) {
    all_configs.push_back(ci);
  }

  std::vector<cmGeneratorTarget::AllConfigSource> const& sources =
    this->GeneratorTarget->GetAllConfigSources();

  for (cmGeneratorTarget::AllConfigSource const& si : sources) {
    std::string tool;
    switch (si.Kind) {
      case cmGeneratorTarget::SourceKindAppManifest:
        tool = "AppxManifest";
        break;
      case cmGeneratorTarget::SourceKindCertificate:
        tool = "None";
        break;
      case cmGeneratorTarget::SourceKindCustomCommand:
        // Handled elsewhere.
        break;
      case cmGeneratorTarget::SourceKindExternalObject:
        tool = "Object";
        if (this->LocalGenerator->GetVersion() <
            cmGlobalVisualStudioGenerator::VS11) {
          // For VS == 10 we cannot use LinkObjects to avoid linking custom
          // command outputs.  If an object file is generated in this target,
          // then vs10 will use it in the build, and we have to list it as
          // None instead of Object.
          std::vector<cmSourceFile*> const* d =
            this->GeneratorTarget->GetSourceDepends(si.Source);
          if (d && !d->empty()) {
            tool = "None";
          }
        }
        break;
      case cmGeneratorTarget::SourceKindExtra:
        this->WriteExtraSource(si.Source);
        break;
      case cmGeneratorTarget::SourceKindHeader:
        this->WriteHeaderSource(si.Source);
        break;
      case cmGeneratorTarget::SourceKindIDL:
        tool = "Midl";
        break;
      case cmGeneratorTarget::SourceKindManifest:
        // Handled elsewhere.
        break;
      case cmGeneratorTarget::SourceKindModuleDefinition:
        tool = "None";
        break;
      case cmGeneratorTarget::SourceKindObjectSource: {
        const std::string& lang = si.Source->GetLanguage();
        if (lang == "C" || lang == "CXX") {
          tool = "ClCompile";
        } else if (lang == "ASM_MASM" &&
                   this->GlobalGenerator->IsMasmEnabled()) {
          tool = "MASM";
        } else if (lang == "ASM_NASM" &&
                   this->GlobalGenerator->IsNasmEnabled()) {
          tool = "NASM";
        } else if (lang == "RC") {
          tool = "ResourceCompile";
        } else if (lang == "CSharp") {
          tool = "Compile";
        } else if (lang == "CUDA" && this->GlobalGenerator->IsCudaEnabled()) {
          tool = "CudaCompile";
        } else {
          tool = "None";
        }
      } break;
      case cmGeneratorTarget::SourceKindResx:
        // Handled elsewhere.
        break;
      case cmGeneratorTarget::SourceKindXaml:
        // Handled elsewhere.
        break;
    }

    if (!tool.empty()) {
      // Compute set of configurations to exclude, if any.
      std::vector<size_t> const& include_configs = si.Configs;
      std::vector<size_t> exclude_configs;
      std::set_difference(all_configs.begin(), all_configs.end(),
                          include_configs.begin(), include_configs.end(),
                          std::back_inserter(exclude_configs));

      if (si.Kind == cmGeneratorTarget::SourceKindObjectSource) {
        // FIXME: refactor generation to avoid tracking XML syntax state.
        this->WriteSource(tool, si.Source, "");
        bool have_nested = this->OutputSourceSpecificFlags(si.Source);
        if (!exclude_configs.empty()) {
          if (!have_nested) {
            (*this->BuildFileStream) << ">\n";
          }
          this->WriteExcludeFromBuild(exclude_configs);
          have_nested = true;
        }
        if (have_nested) {
          this->WriteString("</", 2);
          (*this->BuildFileStream) << tool << ">\n";
        } else {
          (*this->BuildFileStream) << " />\n";
        }
      } else if (!exclude_configs.empty()) {
        this->WriteSource(tool, si.Source, ">\n");
        this->WriteExcludeFromBuild(exclude_configs);
        this->WriteString("</", 2);
        (*this->BuildFileStream) << tool << ">\n";
      } else {
        this->WriteSource(tool, si.Source);
      }
    }
  }

  if (this->IsMissingFiles) {
    this->WriteMissingFiles();
  }

  this->WriteString("</ItemGroup>\n", 1);
}

bool cmVisualStudio10TargetGenerator::OutputSourceSpecificFlags(
  cmSourceFile const* source)
{
  cmSourceFile const& sf = *source;

  std::string objectName;
  if (this->GeneratorTarget->HasExplicitObjectName(&sf)) {
    objectName = this->GeneratorTarget->GetObjectName(&sf);
  }
  std::string flags;
  bool configDependentFlags = false;
  std::string options;
  bool configDependentOptions = false;
  std::string defines;
  bool configDependentDefines = false;
  std::string includes;
  bool configDependentIncludes = false;
  if (const char* cflags = sf.GetProperty("COMPILE_FLAGS")) {
    configDependentFlags =
      cmGeneratorExpression::Find(cflags) != std::string::npos;
    flags += cflags;
  }
  if (const char* coptions = sf.GetProperty("COMPILE_OPTIONS")) {
    configDependentOptions =
      cmGeneratorExpression::Find(coptions) != std::string::npos;
    options += coptions;
  }
  if (const char* cdefs = sf.GetProperty("COMPILE_DEFINITIONS")) {
    configDependentDefines =
      cmGeneratorExpression::Find(cdefs) != std::string::npos;
    defines += cdefs;
  }
  if (const char* cincludes = sf.GetProperty("INCLUDE_DIRECTORIES")) {
    configDependentIncludes =
      cmGeneratorExpression::Find(cincludes) != std::string::npos;
    includes += cincludes;
  }
  std::string lang =
    this->GlobalGenerator->GetLanguageFromExtension(sf.GetExtension().c_str());
  std::string sourceLang = this->LocalGenerator->GetSourceFileLanguage(sf);
  const std::string& linkLanguage =
    this->GeneratorTarget->GetLinkerLanguage("");
  bool needForceLang = false;
  // source file does not match its extension language
  if (lang != sourceLang) {
    needForceLang = true;
    lang = sourceLang;
  }
  // if the source file does not match the linker language
  // then force c or c++
  const char* compileAs = 0;
  if (needForceLang || (linkLanguage != lang)) {
    if (lang == "CXX") {
      // force a C++ file type
      compileAs = "CompileAsCpp";
    } else if (lang == "C") {
      // force to c
      compileAs = "CompileAsC";
    }
  }
  bool noWinRT = this->TargetCompileAsWinRT && lang == "C";
  bool hasFlags = false;
  // for the first time we need a new line if there is something
  // produced here.
  const char* firstString = ">\n";
  if (!objectName.empty()) {
    (*this->BuildFileStream) << firstString;
    firstString = "";
    hasFlags = true;
    if (lang == "CUDA") {
      this->WriteString("<CompileOut>", 3);
      (*this->BuildFileStream) << "$(IntDir)/" << objectName
                               << "</CompileOut>\n";
    } else {
      this->WriteString("<ObjectFileName>", 3);
      (*this->BuildFileStream) << "$(IntDir)/" << objectName
                               << "</ObjectFileName>\n";
    }
  }
  for (std::string const& config : this->Configurations) {
    std::string configUpper = cmSystemTools::UpperCase(config);
    std::string configDefines = defines;
    std::string defPropName = "COMPILE_DEFINITIONS_";
    defPropName += configUpper;
    if (const char* ccdefs = sf.GetProperty(defPropName)) {
      if (!configDefines.empty()) {
        configDefines += ";";
      }
      configDependentDefines |=
        cmGeneratorExpression::Find(ccdefs) != std::string::npos;
      configDefines += ccdefs;
    }
    // if we have flags or defines for this config then
    // use them
    if (!flags.empty() || !options.empty() || !configDefines.empty() ||
        !includes.empty() || compileAs || noWinRT) {
      (*this->BuildFileStream) << firstString;
      firstString = ""; // only do firstString once
      hasFlags = true;
      cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
      cmIDEFlagTable const* flagtable = nullptr;
      const std::string& srclang = source->GetLanguage();
      if (srclang == "C" || srclang == "CXX") {
        flagtable = gg->GetClFlagTable();
      } else if (srclang == "ASM_MASM" &&
                 this->GlobalGenerator->IsMasmEnabled()) {
        flagtable = gg->GetMasmFlagTable();
      } else if (lang == "ASM_NASM" &&
                 this->GlobalGenerator->IsNasmEnabled()) {
        flagtable = gg->GetNasmFlagTable();
      } else if (srclang == "RC") {
        flagtable = gg->GetRcFlagTable();
      } else if (srclang == "CSharp") {
        flagtable = gg->GetCSharpFlagTable();
      }
      cmGeneratorExpressionInterpreter genexInterpreter(
        this->LocalGenerator, this->GeneratorTarget, config,
        this->GeneratorTarget->GetName(), lang);
      cmVisualStudioGeneratorOptions clOptions(
        this->LocalGenerator, cmVisualStudioGeneratorOptions::Compiler,
        flagtable, 0, this);
      if (compileAs) {
        clOptions.AddFlag("CompileAs", compileAs);
      }
      if (noWinRT) {
        clOptions.AddFlag("CompileAsWinRT", "false");
      }
      if (configDependentFlags) {
        clOptions.Parse(genexInterpreter.Evaluate(flags, "COMPILE_FLAGS"));
      } else {
        clOptions.Parse(flags.c_str());
      }
      if (!options.empty()) {
        std::string expandedOptions;
        if (configDependentOptions) {
          this->LocalGenerator->AppendCompileOptions(
            expandedOptions,
            genexInterpreter.Evaluate(options, "COMPILE_OPTIONS"));
        } else {
          this->LocalGenerator->AppendCompileOptions(expandedOptions, options);
        }
        clOptions.Parse(expandedOptions.c_str());
      }
      if (clOptions.HasFlag("DisableSpecificWarnings")) {
        clOptions.AppendFlag("DisableSpecificWarnings",
                             "%(DisableSpecificWarnings)");
      }
      if (configDependentDefines) {
        clOptions.AddDefines(
          genexInterpreter.Evaluate(configDefines, "COMPILE_DEFINITIONS"));
      } else {
        clOptions.AddDefines(configDefines);
      }
      std::vector<std::string> includeList;
      if (configDependentIncludes) {
        this->LocalGenerator->AppendIncludeDirectories(
          includeList,
          genexInterpreter.Evaluate(includes, "INCLUDE_DIRECTORIES"), *source);
      } else {
        this->LocalGenerator->AppendIncludeDirectories(includeList, includes,
                                                       *source);
      }
      clOptions.AddIncludes(includeList);
      clOptions.SetConfiguration(config);
      clOptions.PrependInheritedString("AdditionalOptions");
      clOptions.OutputAdditionalIncludeDirectories(*this->BuildFileStream,
                                                   "      ", "\n", lang);
      clOptions.OutputFlagMap(*this->BuildFileStream, "      ");
      clOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                              "\n", lang);
    }
  }
  if (this->IsXamlSource(source->GetFullPath())) {
    (*this->BuildFileStream) << firstString;
    firstString = ""; // only do firstString once
    hasFlags = true;
    this->WriteString("<DependentUpon>", 3);
    const std::string& fileName = source->GetFullPath();
    std::string xamlFileName = fileName.substr(0, fileName.find_last_of("."));
    (*this->BuildFileStream) << xamlFileName << "</DependentUpon>\n";
  }
  if (this->ProjectType == csproj) {
    std::string f = source->GetFullPath();
    typedef std::map<std::string, std::string> CsPropMap;
    CsPropMap sourceFileTags;
    // set <Link> tag if necessary
    std::string link;
    this->GetCSharpSourceLink(source, link);
    if (!link.empty()) {
      sourceFileTags["Link"] = link;
    }
    this->GetCSharpSourceProperties(&sf, sourceFileTags);
    // write source file specific tags
    if (!sourceFileTags.empty()) {
      hasFlags = true;
      (*this->BuildFileStream) << firstString;
      firstString = "";
      this->WriteCSharpSourceProperties(sourceFileTags);
    }
  }

  return hasFlags;
}

void cmVisualStudio10TargetGenerator::WriteExcludeFromBuild(
  std::vector<size_t> const& exclude_configs)
{
  for (size_t ci : exclude_configs) {
    this->WriteString("", 3);
    (*this->BuildFileStream)
      << "<ExcludedFromBuild Condition=\"'$(Configuration)|$(Platform)'=='"
      << cmVS10EscapeXML(this->Configurations[ci]) << "|"
      << cmVS10EscapeXML(this->Platform) << "'\">true</ExcludedFromBuild>\n";
  }
}

void cmVisualStudio10TargetGenerator::WritePathAndIncrementalLinkOptions()
{
  cmStateEnums::TargetType ttype = this->GeneratorTarget->GetType();
  if (ttype > cmStateEnums::GLOBAL_TARGET) {
    return;
  }
  if (this->ProjectType == csproj) {
    return;
  }

  this->WriteString("<PropertyGroup>\n", 1);
  this->WriteString("<_ProjectFileVersion>10.0.20506.1"
                    "</_ProjectFileVersion>\n",
                    2);
  for (std::string const& config : this->Configurations) {
    if (ttype >= cmStateEnums::UTILITY) {
      this->WritePlatformConfigTag("IntDir", config, 2);
      *this->BuildFileStream
        << "$(Platform)\\$(Configuration)\\$(ProjectName)\\"
        << "</IntDir>\n";
    } else {
      std::string intermediateDir =
        this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
      intermediateDir += "/";
      intermediateDir += config;
      intermediateDir += "/";
      std::string outDir;
      std::string targetNameFull;
      if (ttype == cmStateEnums::OBJECT_LIBRARY) {
        outDir = intermediateDir;
        targetNameFull = this->GeneratorTarget->GetName();
        targetNameFull += ".lib";
      } else {
        outDir = this->GeneratorTarget->GetDirectory(config) + "/";
        targetNameFull = this->GeneratorTarget->GetFullName(config);
      }
      ConvertToWindowsSlash(intermediateDir);
      ConvertToWindowsSlash(outDir);

      this->WritePlatformConfigTag("OutDir", config, 2);
      *this->BuildFileStream << cmVS10EscapeXML(outDir) << "</OutDir>\n";

      this->WritePlatformConfigTag("IntDir", config, 2);
      *this->BuildFileStream << cmVS10EscapeXML(intermediateDir)
                             << "</IntDir>\n";

      if (const char* workingDir = this->GeneratorTarget->GetProperty(
            "VS_DEBUGGER_WORKING_DIRECTORY")) {
        this->WritePlatformConfigTag("LocalDebuggerWorkingDirectory", config,
                                     2);
        *this->BuildFileStream << cmVS10EscapeXML(workingDir)
                               << "</LocalDebuggerWorkingDirectory>\n";
      }

      std::string name =
        cmSystemTools::GetFilenameWithoutLastExtension(targetNameFull);
      this->WritePlatformConfigTag("TargetName", config, 2);
      *this->BuildFileStream << cmVS10EscapeXML(name) << "</TargetName>\n";

      std::string ext =
        cmSystemTools::GetFilenameLastExtension(targetNameFull);
      if (ext.empty()) {
        // An empty TargetExt causes a default extension to be used.
        // A single "." appears to be treated as an empty extension.
        ext = ".";
      }
      this->WritePlatformConfigTag("TargetExt", config, 2);
      *this->BuildFileStream << cmVS10EscapeXML(ext) << "</TargetExt>\n";

      this->OutputLinkIncremental(config);
    }
  }
  this->WriteString("</PropertyGroup>\n", 1);
}

void cmVisualStudio10TargetGenerator::OutputLinkIncremental(
  std::string const& configName)
{
  if (!this->MSTools) {
    return;
  }
  if (this->ProjectType == csproj) {
    return;
  }
  // static libraries and things greater than modules do not need
  // to set this option
  if (this->GeneratorTarget->GetType() == cmStateEnums::STATIC_LIBRARY ||
      this->GeneratorTarget->GetType() > cmStateEnums::MODULE_LIBRARY) {
    return;
  }
  Options& linkOptions = *(this->LinkOptions[configName]);

  const char* incremental = linkOptions.GetFlag("LinkIncremental");
  this->WritePlatformConfigTag("LinkIncremental", configName, 2);
  *this->BuildFileStream << (incremental ? incremental : "true")
                         << "</LinkIncremental>\n";
  linkOptions.RemoveFlag("LinkIncremental");

  const char* manifest = linkOptions.GetFlag("GenerateManifest");
  this->WritePlatformConfigTag("GenerateManifest", configName, 2);
  *this->BuildFileStream << (manifest ? manifest : "true")
                         << "</GenerateManifest>\n";
  linkOptions.RemoveFlag("GenerateManifest");

  // Some link options belong here.  Use them now and remove them so that
  // WriteLinkOptions does not use them.
  const char* flags[] = { "LinkDelaySign", "LinkKeyFile", 0 };
  for (const char** f = flags; *f; ++f) {
    const char* flag = *f;
    if (const char* value = linkOptions.GetFlag(flag)) {
      this->WritePlatformConfigTag(flag, configName, 2);
      *this->BuildFileStream << value << "</" << flag << ">\n";
      linkOptions.RemoveFlag(flag);
    }
  }
}

std::vector<std::string> cmVisualStudio10TargetGenerator::GetIncludes(
  std::string const& config, std::string const& lang) const
{
  std::vector<std::string> includes;
  this->LocalGenerator->GetIncludeDirectories(includes, this->GeneratorTarget,
                                              lang, config);
  for (std::string& i : includes) {
    ConvertToWindowsSlash(i);
  }
  return includes;
}

bool cmVisualStudio10TargetGenerator::ComputeClOptions()
{
  for (std::string const& i : this->Configurations) {
    if (!this->ComputeClOptions(i)) {
      return false;
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeClOptions(
  std::string const& configName)
{
  // much of this was copied from here:
  // copied from cmLocalVisualStudio7Generator.cxx 805
  // TODO: Integrate code below with cmLocalVisualStudio7Generator.

  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  std::unique_ptr<Options> pOptions;
  switch (this->ProjectType) {
    case vcxproj:
      pOptions = cm::make_unique<Options>(
        this->LocalGenerator, Options::Compiler, gg->GetClFlagTable());
      break;
    case csproj:
      pOptions =
        cm::make_unique<Options>(this->LocalGenerator, Options::CSharpCompiler,
                                 gg->GetCSharpFlagTable());
      break;
  }
  Options& clOptions = *pOptions;

  std::string flags;
  const std::string& linkLanguage =
    this->GeneratorTarget->GetLinkerLanguage(configName);
  if (linkLanguage.empty()) {
    cmSystemTools::Error(
      "CMake can not determine linker language for target: ",
      this->Name.c_str());
    return false;
  }

  // Choose a language whose flags to use for ClCompile.
  static const char* clLangs[] = { "CXX", "C", "Fortran", "CSharp" };
  std::string langForClCompile;
  if (std::find(cm::cbegin(clLangs), cm::cend(clLangs), linkLanguage) !=
      cm::cend(clLangs)) {
    langForClCompile = linkLanguage;
  } else {
    std::set<std::string> languages;
    this->GeneratorTarget->GetLanguages(languages, configName);
    for (const char* const* l = cm::cbegin(clLangs); l != cm::cend(clLangs);
         ++l) {
      if (languages.find(*l) != languages.end()) {
        langForClCompile = *l;
        break;
      }
    }
  }
  this->LangForClCompile = langForClCompile;
  if (!langForClCompile.empty()) {
    std::string baseFlagVar = "CMAKE_";
    baseFlagVar += langForClCompile;
    baseFlagVar += "_FLAGS";
    flags = this->Makefile->GetRequiredDefinition(baseFlagVar);
    std::string flagVar =
      baseFlagVar + std::string("_") + cmSystemTools::UpperCase(configName);
    flags += " ";
    flags += this->Makefile->GetRequiredDefinition(flagVar);
    this->LocalGenerator->AddCompileOptions(flags, this->GeneratorTarget,
                                            langForClCompile, configName);
  }
  // set the correct language
  if (linkLanguage == "C") {
    clOptions.AddFlag("CompileAs", "CompileAsC");
  }
  if (linkLanguage == "CXX") {
    clOptions.AddFlag("CompileAs", "CompileAsCpp");
  }

  // Check IPO related warning/error.
  this->GeneratorTarget->IsIPOEnabled(linkLanguage, configName);

  // Get preprocessor definitions for this directory.
  std::string defineFlags = this->Makefile->GetDefineFlags();
  if (this->MSTools) {
    if (this->ProjectType == vcxproj) {
      clOptions.FixExceptionHandlingDefault();
      clOptions.AddFlag("PrecompiledHeader", "NotUsing");
      std::string asmLocation = configName + "/";
      clOptions.AddFlag("AssemblerListingLocation", asmLocation);
    }
  }
  clOptions.Parse(flags.c_str());
  clOptions.Parse(defineFlags.c_str());
  std::vector<std::string> targetDefines;
  switch (this->ProjectType) {
    case vcxproj:
      if (!langForClCompile.empty()) {
        this->GeneratorTarget->GetCompileDefinitions(targetDefines, configName,
                                                     langForClCompile);
      }
      break;
    case csproj:
      this->GeneratorTarget->GetCompileDefinitions(targetDefines, configName,
                                                   "CSharp");
      break;
  }
  clOptions.AddDefines(targetDefines);

  // Get includes for this target
  if (!this->LangForClCompile.empty()) {
    clOptions.AddIncludes(
      this->GetIncludes(configName, this->LangForClCompile));
  }

  if (this->MSTools) {
    clOptions.SetVerboseMakefile(
      this->Makefile->IsOn("CMAKE_VERBOSE_MAKEFILE"));
  }

  // Add a definition for the configuration name.
  std::string configDefine = "CMAKE_INTDIR=\"";
  configDefine += configName;
  configDefine += "\"";
  clOptions.AddDefine(configDefine);
  if (const char* exportMacro = this->GeneratorTarget->GetExportMacro()) {
    clOptions.AddDefine(exportMacro);
  }

  if (this->MSTools) {
    // If we have the VS_WINRT_COMPONENT set then force Compile as WinRT.
    if (this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_COMPONENT")) {
      clOptions.AddFlag("CompileAsWinRT", "true");
      // For WinRT components, add the _WINRT_DLL define to produce a lib
      if (this->GeneratorTarget->GetType() == cmStateEnums::SHARED_LIBRARY ||
          this->GeneratorTarget->GetType() == cmStateEnums::MODULE_LIBRARY) {
        clOptions.AddDefine("_WINRT_DLL");
      }
    } else if (this->GlobalGenerator->TargetsWindowsStore() ||
               this->GlobalGenerator->TargetsWindowsPhone()) {
      if (!clOptions.IsWinRt()) {
        clOptions.AddFlag("CompileAsWinRT", "false");
      }
    }
    if (const char* winRT = clOptions.GetFlag("CompileAsWinRT")) {
      if (cmSystemTools::IsOn(winRT)) {
        this->TargetCompileAsWinRT = true;
      }
    }
  }

  if (this->ProjectType != csproj && clOptions.IsManaged()) {
    this->Managed = true;
    std::string managedType = clOptions.GetFlag("CompileAsManaged");
    if (managedType == "Safe") {
      // force empty calling convention if safe clr is used
      clOptions.AddFlag("CallingConvention", "");
    }
  }
  if (this->ProjectType == csproj) {
    // /nowin32manifest overrides /win32manifest: parameter
    if (clOptions.HasFlag("NoWin32Manifest")) {
      clOptions.RemoveFlag("ApplicationManifest");
    }
  }

  this->ClOptions[configName] = std::move(pOptions);
  return true;
}

void cmVisualStudio10TargetGenerator::WriteClOptions(
  std::string const& configName)
{
  Options& clOptions = *(this->ClOptions[configName]);
  if (this->ProjectType == csproj) {
    return;
  }
  this->WriteString("<ClCompile>\n", 2);
  clOptions.PrependInheritedString("AdditionalOptions");
  clOptions.OutputAdditionalIncludeDirectories(
    *this->BuildFileStream, "      ", "\n", this->LangForClCompile);
  clOptions.OutputFlagMap(*this->BuildFileStream, "      ");
  clOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                          "\n", this->LangForClCompile);

  if (this->NsightTegra) {
    if (const char* processMax =
          this->GeneratorTarget->GetProperty("ANDROID_PROCESS_MAX")) {
      this->WriteString("<ProcessMax>", 3);
      *this->BuildFileStream << cmVS10EscapeXML(processMax)
                             << "</ProcessMax>\n";
    }
  }

  if (this->MSTools) {
    cmsys::RegularExpression clangToolset("v[0-9]+_clang_.*");
    const char* toolset = this->GlobalGenerator->GetPlatformToolset();
    if (toolset && clangToolset.find(toolset)) {
      this->WriteString("<ObjectFileName>"
                        "$(IntDir)%(filename).obj"
                        "</ObjectFileName>\n",
                        3);
    } else {
      this->WriteString("<ObjectFileName>$(IntDir)</ObjectFileName>\n", 3);
    }

    // If not in debug mode, write the DebugInformationFormat field
    // without value so PDBs don't get generated uselessly. Each tag
    // goes on its own line because Visual Studio corrects it this
    // way when saving the project after CMake generates it.
    if (!clOptions.IsDebug()) {
      this->WriteString("<DebugInformationFormat>\n", 3);
      this->WriteString("</DebugInformationFormat>\n", 3);
    }

    // Specify the compiler program database file if configured.
    std::string pdb = this->GeneratorTarget->GetCompilePDBPath(configName);
    if (!pdb.empty()) {
      ConvertToWindowsSlash(pdb);
      this->WriteString("<ProgramDataBaseFileName>", 3);
      *this->BuildFileStream << cmVS10EscapeXML(pdb)
                             << "</ProgramDataBaseFileName>\n";
    }
  }

  this->WriteString("</ClCompile>\n", 2);
}

bool cmVisualStudio10TargetGenerator::ComputeRcOptions()
{
  for (std::string const& i : this->Configurations) {
    if (!this->ComputeRcOptions(i)) {
      return false;
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeRcOptions(
  std::string const& configName)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  auto pOptions = cm::make_unique<Options>(
    this->LocalGenerator, Options::ResourceCompiler, gg->GetRcFlagTable());
  Options& rcOptions = *pOptions;

  std::string CONFIG = cmSystemTools::UpperCase(configName);
  std::string rcConfigFlagsVar = std::string("CMAKE_RC_FLAGS_") + CONFIG;
  std::string flags =
    std::string(this->Makefile->GetSafeDefinition("CMAKE_RC_FLAGS")) +
    std::string(" ") +
    std::string(this->Makefile->GetSafeDefinition(rcConfigFlagsVar));

  rcOptions.Parse(flags.c_str());

  // For historical reasons, add the C preprocessor defines to RC.
  Options& clOptions = *(this->ClOptions[configName]);
  rcOptions.AddDefines(clOptions.GetDefines());

  // Get includes for this target
  rcOptions.AddIncludes(this->GetIncludes(configName, "RC"));

  this->RcOptions[configName] = std::move(pOptions);
  return true;
}

void cmVisualStudio10TargetGenerator::WriteRCOptions(
  std::string const& configName)
{
  if (!this->MSTools) {
    return;
  }
  this->WriteString("<ResourceCompile>\n", 2);

  Options& rcOptions = *(this->RcOptions[configName]);
  rcOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                          "\n", "RC");
  rcOptions.OutputAdditionalIncludeDirectories(*this->BuildFileStream,
                                               "      ", "\n", "RC");
  rcOptions.PrependInheritedString("AdditionalOptions");
  rcOptions.OutputFlagMap(*this->BuildFileStream, "      ");

  this->WriteString("</ResourceCompile>\n", 2);
}

bool cmVisualStudio10TargetGenerator::ComputeCudaOptions()
{
  if (!this->GlobalGenerator->IsCudaEnabled()) {
    return true;
  }
  for (std::string const& i : this->Configurations) {
    if (!this->ComputeCudaOptions(i)) {
      return false;
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeCudaOptions(
  std::string const& configName)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  auto pOptions = cm::make_unique<Options>(
    this->LocalGenerator, Options::CudaCompiler, gg->GetCudaFlagTable());
  Options& cudaOptions = *pOptions;

  // Get compile flags for CUDA in this directory.
  std::string CONFIG = cmSystemTools::UpperCase(configName);
  std::string configFlagsVar = std::string("CMAKE_CUDA_FLAGS_") + CONFIG;
  std::string flags =
    std::string(this->Makefile->GetSafeDefinition("CMAKE_CUDA_FLAGS")) +
    std::string(" ") +
    std::string(this->Makefile->GetSafeDefinition(configFlagsVar));
  this->LocalGenerator->AddCompileOptions(flags, this->GeneratorTarget, "CUDA",
                                          configName);

  // Get preprocessor definitions for this directory.
  std::string defineFlags = this->Makefile->GetDefineFlags();

  cudaOptions.Parse(flags.c_str());
  cudaOptions.Parse(defineFlags.c_str());
  cudaOptions.ParseFinish();

  // If we haven't explicitly enabled GPU debug information
  // explicitly disable it
  if (!cudaOptions.HasFlag("GPUDebugInfo")) {
    cudaOptions.AddFlag("GPUDebugInfo", "false");
  }

  // The extension on object libraries the CUDA gives isn't
  // consistent with how MSVC generates object libraries for C+, so set
  // the default to not have any extension
  cudaOptions.AddFlag("CompileOut", "$(IntDir)%(Filename).obj");

  bool notPtx = true;
  if (this->GeneratorTarget->GetPropertyAsBool("CUDA_SEPARABLE_COMPILATION")) {
    cudaOptions.AddFlag("GenerateRelocatableDeviceCode", "true");
  } else if (this->GeneratorTarget->GetPropertyAsBool(
               "CUDA_PTX_COMPILATION")) {
    cudaOptions.AddFlag("NvccCompilation", "ptx");
    // We drop the %(Extension) component as CMake expects all PTX files
    // to not have the source file extension at all
    cudaOptions.AddFlag("CompileOut", "$(IntDir)%(Filename).ptx");
    notPtx = false;
  }

  if (notPtx &&
      cmSystemTools::VersionCompareGreaterEq(
        "8.0", this->GlobalGenerator->GetPlatformToolsetCudaString())) {
    // Explicitly state that we want this file to be treated as a
    // CUDA file no matter what the file extensions is
    // This is only needed for < CUDA 9
    cudaOptions.AppendFlagString("AdditionalOptions", "-x cu");
  }

  // CUDA automatically passes the proper '--machine' flag to nvcc
  // for the current architecture, but does not reflect this default
  // in the user-visible IDE settings.  Set it explicitly.
  if (this->Platform == "x64") {
    cudaOptions.AddFlag("TargetMachinePlatform", "64");
  }

  // Convert the host compiler options to the toolset's abstractions
  // using a secondary flag table.
  cudaOptions.ClearTables();
  cudaOptions.AddTable(gg->GetCudaHostFlagTable());
  cudaOptions.Reparse("AdditionalCompilerOptions");

  // `CUDA 8.0.targets` places AdditionalCompilerOptions before nvcc!
  // Pass them through -Xcompiler in AdditionalOptions instead.
  if (const char* acoPtr = cudaOptions.GetFlag("AdditionalCompilerOptions")) {
    std::string aco = acoPtr;
    cudaOptions.RemoveFlag("AdditionalCompilerOptions");
    if (!aco.empty()) {
      aco = this->LocalGenerator->EscapeForShell(aco, false);
      cudaOptions.AppendFlagString("AdditionalOptions", "-Xcompiler=" + aco);
    }
  }

  cudaOptions.FixCudaCodeGeneration();

  std::vector<std::string> targetDefines;
  this->GeneratorTarget->GetCompileDefinitions(targetDefines, configName,
                                               "CUDA");
  cudaOptions.AddDefines(targetDefines);

  // Add a definition for the configuration name.
  std::string configDefine = "CMAKE_INTDIR=\"";
  configDefine += configName;
  configDefine += "\"";
  cudaOptions.AddDefine(configDefine);
  if (const char* exportMacro = this->GeneratorTarget->GetExportMacro()) {
    cudaOptions.AddDefine(exportMacro);
  }

  // Get includes for this target
  cudaOptions.AddIncludes(this->GetIncludes(configName, "CUDA"));

  this->CudaOptions[configName] = std::move(pOptions);
  return true;
}

void cmVisualStudio10TargetGenerator::WriteCudaOptions(
  std::string const& configName)
{
  if (!this->MSTools || !this->GlobalGenerator->IsCudaEnabled()) {
    return;
  }
  this->WriteString("<CudaCompile>\n", 2);

  Options& cudaOptions = *(this->CudaOptions[configName]);
  cudaOptions.OutputAdditionalIncludeDirectories(*this->BuildFileStream,
                                                 "      ", "\n", "CUDA");
  cudaOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                            "\n", "CUDA");
  cudaOptions.PrependInheritedString("AdditionalOptions");
  cudaOptions.OutputFlagMap(*this->BuildFileStream, "      ");

  this->WriteString("</CudaCompile>\n", 2);
}

bool cmVisualStudio10TargetGenerator::ComputeCudaLinkOptions()
{
  if (!this->GlobalGenerator->IsCudaEnabled()) {
    return true;
  }
  for (std::string const& i : this->Configurations) {
    if (!this->ComputeCudaLinkOptions(i)) {
      return false;
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeCudaLinkOptions(
  std::string const& configName)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  auto pOptions = cm::make_unique<Options>(
    this->LocalGenerator, Options::CudaCompiler, gg->GetCudaFlagTable());
  Options& cudaLinkOptions = *pOptions;

  // Determine if we need to do a device link
  bool doDeviceLinking = false;
  switch (this->GeneratorTarget->GetType()) {
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
    case cmStateEnums::EXECUTABLE:
      doDeviceLinking = true;
      break;
    case cmStateEnums::STATIC_LIBRARY:
      doDeviceLinking = this->GeneratorTarget->GetPropertyAsBool(
        "CUDA_RESOLVE_DEVICE_SYMBOLS");
      break;
    default:
      break;
  }

  cudaLinkOptions.AddFlag("PerformDeviceLink",
                          doDeviceLinking ? "true" : "false");

  // Suppress deprecation warnings for default GPU targets during device link.
  if (cmSystemTools::VersionCompareGreaterEq(
        this->GlobalGenerator->GetPlatformToolsetCudaString(), "8.0")) {
    cudaLinkOptions.AppendFlagString("AdditionalOptions",
                                     "-Wno-deprecated-gpu-targets");
  }

  this->CudaLinkOptions[configName] = std::move(pOptions);
  return true;
}

void cmVisualStudio10TargetGenerator::WriteCudaLinkOptions(
  std::string const& configName)
{
  if (this->GeneratorTarget->GetType() > cmStateEnums::MODULE_LIBRARY) {
    return;
  }

  if (!this->MSTools || !this->GlobalGenerator->IsCudaEnabled()) {
    return;
  }

  this->WriteString("<CudaLink>\n", 2);
  Options& cudaLinkOptions = *(this->CudaLinkOptions[configName]);
  cudaLinkOptions.OutputFlagMap(*this->BuildFileStream, "      ");
  this->WriteString("</CudaLink>\n", 2);
}

bool cmVisualStudio10TargetGenerator::ComputeMasmOptions()
{
  if (!this->GlobalGenerator->IsMasmEnabled()) {
    return true;
  }
  for (std::string const& i : this->Configurations) {
    if (!this->ComputeMasmOptions(i)) {
      return false;
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeMasmOptions(
  std::string const& configName)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  auto pOptions = cm::make_unique<Options>(
    this->LocalGenerator, Options::MasmCompiler, gg->GetMasmFlagTable());
  Options& masmOptions = *pOptions;

  std::string CONFIG = cmSystemTools::UpperCase(configName);
  std::string configFlagsVar = std::string("CMAKE_ASM_MASM_FLAGS_") + CONFIG;
  std::string flags =
    std::string(this->Makefile->GetSafeDefinition("CMAKE_ASM_MASM_FLAGS")) +
    std::string(" ") +
    std::string(this->Makefile->GetSafeDefinition(configFlagsVar));

  masmOptions.Parse(flags.c_str());

  // Get includes for this target
  masmOptions.AddIncludes(this->GetIncludes(configName, "ASM_MASM"));

  this->MasmOptions[configName] = std::move(pOptions);
  return true;
}

void cmVisualStudio10TargetGenerator::WriteMasmOptions(
  std::string const& configName)
{
  if (!this->MSTools || !this->GlobalGenerator->IsMasmEnabled()) {
    return;
  }
  this->WriteString("<MASM>\n", 2);

  // Preprocessor definitions and includes are shared with clOptions.
  Options& clOptions = *(this->ClOptions[configName]);
  clOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                          "\n", "ASM_MASM");

  Options& masmOptions = *(this->MasmOptions[configName]);
  masmOptions.OutputAdditionalIncludeDirectories(*this->BuildFileStream,
                                                 "      ", "\n", "ASM_MASM");
  masmOptions.PrependInheritedString("AdditionalOptions");
  masmOptions.OutputFlagMap(*this->BuildFileStream, "      ");

  this->WriteString("</MASM>\n", 2);
}

bool cmVisualStudio10TargetGenerator::ComputeNasmOptions()
{
  if (!this->GlobalGenerator->IsNasmEnabled()) {
    return true;
  }
  for (std::string const& i : this->Configurations) {
    if (!this->ComputeNasmOptions(i)) {
      return false;
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeNasmOptions(
  std::string const& configName)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  auto pOptions = cm::make_unique<Options>(
    this->LocalGenerator, Options::NasmCompiler, gg->GetNasmFlagTable());
  Options& nasmOptions = *pOptions;

  std::string CONFIG = cmSystemTools::UpperCase(configName);
  std::string configFlagsVar = std::string("CMAKE_ASM_NASM_FLAGS_") + CONFIG;
  std::string flags =
    std::string(this->Makefile->GetSafeDefinition("CMAKE_ASM_NASM_FLAGS")) +
    std::string(" -f") + std::string(this->Makefile->GetSafeDefinition(
                           "CMAKE_ASM_NASM_OBJECT_FORMAT")) +
    std::string(" ") +
    std::string(this->Makefile->GetSafeDefinition(configFlagsVar));
  nasmOptions.Parse(flags.c_str());

  // Get includes for this target
  nasmOptions.AddIncludes(this->GetIncludes(configName, "ASM_NASM"));

  this->NasmOptions[configName] = std::move(pOptions);
  return true;
}

void cmVisualStudio10TargetGenerator::WriteNasmOptions(
  std::string const& configName)
{
  if (!this->GlobalGenerator->IsNasmEnabled()) {
    return;
  }
  this->WriteString("<NASM>\n", 2);

  std::vector<std::string> includes =
    this->GetIncludes(configName, "ASM_NASM");
  Options& nasmOptions = *(this->NasmOptions[configName]);
  nasmOptions.OutputAdditionalIncludeDirectories(*this->BuildFileStream,
                                                 "      ", "\n", "ASM_NASM");
  nasmOptions.OutputFlagMap(*this->BuildFileStream, "      ");
  nasmOptions.PrependInheritedString("AdditionalOptions");
  nasmOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                            "\n", "ASM_NASM");

  // Preprocessor definitions and includes are shared with clOptions.
  Options& clOptions = *(this->ClOptions[configName]);
  clOptions.OutputPreprocessorDefinitions(*this->BuildFileStream, "      ",
                                          "\n", "ASM_NASM");

  this->WriteString("</NASM>\n", 2);
}

void cmVisualStudio10TargetGenerator::WriteLibOptions(
  std::string const& config)
{
  if (this->GeneratorTarget->GetType() != cmStateEnums::STATIC_LIBRARY &&
      this->GeneratorTarget->GetType() != cmStateEnums::OBJECT_LIBRARY) {
    return;
  }
  std::string libflags;
  this->LocalGenerator->GetStaticLibraryFlags(
    libflags, cmSystemTools::UpperCase(config), this->GeneratorTarget);
  if (!libflags.empty()) {
    this->WriteString("<Lib>\n", 2);
    cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
    cmVisualStudioGeneratorOptions libOptions(
      this->LocalGenerator, cmVisualStudioGeneratorOptions::Linker,
      gg->GetLibFlagTable(), 0, this);
    libOptions.Parse(libflags.c_str());
    libOptions.PrependInheritedString("AdditionalOptions");
    libOptions.OutputFlagMap(*this->BuildFileStream, "      ");
    this->WriteString("</Lib>\n", 2);
  }

  // We cannot generate metadata for static libraries.  WindowsPhone
  // and WindowsStore tools look at GenerateWindowsMetadata in the
  // Link tool options even for static libraries.
  if (this->GlobalGenerator->TargetsWindowsPhone() ||
      this->GlobalGenerator->TargetsWindowsStore()) {
    this->WriteString("<Link>\n", 2);
    this->WriteString("<GenerateWindowsMetadata>false"
                      "</GenerateWindowsMetadata>\n",
                      3);
    this->WriteString("</Link>\n", 2);
  }
}

void cmVisualStudio10TargetGenerator::WriteManifestOptions(
  std::string const& config)
{
  if (this->GeneratorTarget->GetType() != cmStateEnums::EXECUTABLE &&
      this->GeneratorTarget->GetType() != cmStateEnums::SHARED_LIBRARY &&
      this->GeneratorTarget->GetType() != cmStateEnums::MODULE_LIBRARY) {
    return;
  }

  std::vector<cmSourceFile const*> manifest_srcs;
  this->GeneratorTarget->GetManifests(manifest_srcs, config);
  if (!manifest_srcs.empty()) {
    this->WriteString("<Manifest>\n", 2);
    this->WriteString("<AdditionalManifestFiles>", 3);
    for (cmSourceFile const* mi : manifest_srcs) {
      std::string m = this->ConvertPath(mi->GetFullPath(), false);
      ConvertToWindowsSlash(m);
      (*this->BuildFileStream) << m << ";";
    }
    (*this->BuildFileStream) << "</AdditionalManifestFiles>\n";
    this->WriteString("</Manifest>\n", 2);
  }
}

void cmVisualStudio10TargetGenerator::WriteAntBuildOptions(
  std::string const& configName)
{
  // Look through the sources for AndroidManifest.xml and use
  // its location as the root source directory.
  std::string rootDir = this->LocalGenerator->GetCurrentSourceDirectory();
  {
    std::vector<cmSourceFile const*> extraSources;
    this->GeneratorTarget->GetExtraSources(extraSources, "");
    for (cmSourceFile const* si : extraSources) {
      if ("androidmanifest.xml" ==
          cmSystemTools::LowerCase(si->GetLocation().GetName())) {
        rootDir = si->GetLocation().GetDirectory();
        break;
      }
    }
  }

  // Tell MSBuild to launch Ant.
  {
    std::string antBuildPath = rootDir;
    this->WriteString("<AntBuild>\n", 2);
    this->WriteString("<AntBuildPath>", 3);
    ConvertToWindowsSlash(antBuildPath);
    (*this->BuildFileStream) << cmVS10EscapeXML(antBuildPath)
                             << "</AntBuildPath>\n";
  }

  if (this->GeneratorTarget->GetPropertyAsBool("ANDROID_SKIP_ANT_STEP")) {
    this->WriteString("<SkipAntStep>true</SkipAntStep>\n", 3);
  }

  if (this->GeneratorTarget->GetPropertyAsBool("ANDROID_PROGUARD")) {
    this->WriteString("<EnableProGuard>true</EnableProGuard>\n", 3);
  }

  if (const char* proGuardConfigLocation =
        this->GeneratorTarget->GetProperty("ANDROID_PROGUARD_CONFIG_PATH")) {
    this->WriteString("<ProGuardConfigLocation>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(proGuardConfigLocation)
                             << "</ProGuardConfigLocation>\n";
  }

  if (const char* securePropertiesLocation =
        this->GeneratorTarget->GetProperty("ANDROID_SECURE_PROPS_PATH")) {
    this->WriteString("<SecurePropertiesLocation>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(securePropertiesLocation)
                             << "</SecurePropertiesLocation>\n";
  }

  if (const char* nativeLibDirectoriesExpression =
        this->GeneratorTarget->GetProperty("ANDROID_NATIVE_LIB_DIRECTORIES")) {
    cmGeneratorExpression ge;
    std::unique_ptr<cmCompiledGeneratorExpression> cge =
      ge.Parse(nativeLibDirectoriesExpression);
    std::string nativeLibDirs =
      cge->Evaluate(this->LocalGenerator, configName);
    this->WriteString("<NativeLibDirectories>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(nativeLibDirs)
                             << "</NativeLibDirectories>\n";
  }

  if (const char* nativeLibDependenciesExpression =
        this->GeneratorTarget->GetProperty(
          "ANDROID_NATIVE_LIB_DEPENDENCIES")) {
    cmGeneratorExpression ge;
    std::unique_ptr<cmCompiledGeneratorExpression> cge =
      ge.Parse(nativeLibDependenciesExpression);
    std::string nativeLibDeps =
      cge->Evaluate(this->LocalGenerator, configName);
    this->WriteString("<NativeLibDependencies>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(nativeLibDeps)
                             << "</NativeLibDependencies>\n";
  }

  if (const char* javaSourceDir =
        this->GeneratorTarget->GetProperty("ANDROID_JAVA_SOURCE_DIR")) {
    this->WriteString("<JavaSourceDir>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(javaSourceDir)
                             << "</JavaSourceDir>\n";
  }

  if (const char* jarDirectoriesExpression =
        this->GeneratorTarget->GetProperty("ANDROID_JAR_DIRECTORIES")) {
    cmGeneratorExpression ge;
    std::unique_ptr<cmCompiledGeneratorExpression> cge =
      ge.Parse(jarDirectoriesExpression);
    std::string jarDirectories =
      cge->Evaluate(this->LocalGenerator, configName);
    this->WriteString("<JarDirectories>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(jarDirectories)
                             << "</JarDirectories>\n";
  }

  if (const char* jarDeps =
        this->GeneratorTarget->GetProperty("ANDROID_JAR_DEPENDENCIES")) {
    this->WriteString("<JarDependencies>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(jarDeps)
                             << "</JarDependencies>\n";
  }

  if (const char* assetsDirectories =
        this->GeneratorTarget->GetProperty("ANDROID_ASSETS_DIRECTORIES")) {
    this->WriteString("<AssetsDirectories>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(assetsDirectories)
                             << "</AssetsDirectories>\n";
  }

  {
    std::string manifest_xml = rootDir + "/AndroidManifest.xml";
    ConvertToWindowsSlash(manifest_xml);
    this->WriteString("<AndroidManifestLocation>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(manifest_xml)
                             << "</AndroidManifestLocation>\n";
  }

  if (const char* antAdditionalOptions =
        this->GeneratorTarget->GetProperty("ANDROID_ANT_ADDITIONAL_OPTIONS")) {
    this->WriteString("<AdditionalOptions>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(antAdditionalOptions)
                             << " %(AdditionalOptions)</AdditionalOptions>\n";
  }

  this->WriteString("</AntBuild>\n", 2);
}

bool cmVisualStudio10TargetGenerator::ComputeLinkOptions()
{
  if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE ||
      this->GeneratorTarget->GetType() == cmStateEnums::SHARED_LIBRARY ||
      this->GeneratorTarget->GetType() == cmStateEnums::MODULE_LIBRARY) {
    for (std::string const& i : this->Configurations) {
      if (!this->ComputeLinkOptions(i)) {
        return false;
      }
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeLinkOptions(
  std::string const& config)
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  auto pOptions =
    cm::make_unique<Options>(this->LocalGenerator, Options::Linker,
                             gg->GetLinkFlagTable(), nullptr, this);
  Options& linkOptions = *pOptions;

  cmGeneratorTarget::LinkClosure const* linkClosure =
    this->GeneratorTarget->GetLinkClosure(config);

  const std::string& linkLanguage = linkClosure->LinkerLanguage;
  if (linkLanguage.empty()) {
    cmSystemTools::Error(
      "CMake can not determine linker language for target: ",
      this->Name.c_str());
    return false;
  }

  std::string CONFIG = cmSystemTools::UpperCase(config);

  const char* linkType = "SHARED";
  if (this->GeneratorTarget->GetType() == cmStateEnums::MODULE_LIBRARY) {
    linkType = "MODULE";
  }
  if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE) {
    linkType = "EXE";
  }
  std::string flags;
  std::string linkFlagVarBase = "CMAKE_";
  linkFlagVarBase += linkType;
  linkFlagVarBase += "_LINKER_FLAGS";
  flags += " ";
  flags += this->Makefile->GetRequiredDefinition(linkFlagVarBase);
  std::string linkFlagVar = linkFlagVarBase + "_" + CONFIG;
  flags += " ";
  flags += this->Makefile->GetRequiredDefinition(linkFlagVar);
  const char* targetLinkFlags =
    this->GeneratorTarget->GetProperty("LINK_FLAGS");
  if (targetLinkFlags) {
    flags += " ";
    flags += targetLinkFlags;
  }
  std::string flagsProp = "LINK_FLAGS_";
  flagsProp += CONFIG;
  if (const char* flagsConfig =
        this->GeneratorTarget->GetProperty(flagsProp)) {
    flags += " ";
    flags += flagsConfig;
  }

  cmComputeLinkInformation* pcli =
    this->GeneratorTarget->GetLinkInformation(config);
  if (!pcli) {
    cmSystemTools::Error(
      "CMake can not compute cmComputeLinkInformation for target: ",
      this->Name.c_str());
    return false;
  }
  cmComputeLinkInformation& cli = *pcli;

  std::vector<std::string> libVec;
  std::vector<std::string> vsTargetVec;
  this->AddLibraries(cli, libVec, vsTargetVec);
  if (std::find(linkClosure->Languages.begin(), linkClosure->Languages.end(),
                "CUDA") != linkClosure->Languages.end()) {
    switch (this->CudaOptions[config]->GetCudaRuntime()) {
      case cmVisualStudioGeneratorOptions::CudaRuntimeStatic:
        libVec.push_back("cudart_static.lib");
        break;
      case cmVisualStudioGeneratorOptions::CudaRuntimeShared:
        libVec.push_back("cudart.lib");
        break;
      case cmVisualStudioGeneratorOptions::CudaRuntimeNone:
        break;
    }
  }
  std::string standardLibsVar = "CMAKE_";
  standardLibsVar += linkLanguage;
  standardLibsVar += "_STANDARD_LIBRARIES";
  std::string const libs = this->Makefile->GetSafeDefinition(standardLibsVar);
  cmSystemTools::ParseWindowsCommandLine(libs.c_str(), libVec);
  linkOptions.AddFlag("AdditionalDependencies", libVec);

  // Populate TargetsFileAndConfigsVec
  for (std::string const& ti : vsTargetVec) {
    this->AddTargetsFileAndConfigPair(ti, config);
  }

  std::vector<std::string> const& ldirs = cli.GetDirectories();
  std::vector<std::string> linkDirs;
  for (std::string const& d : ldirs) {
    // first just full path
    linkDirs.push_back(d);
    // next path with configuration type Debug, Release, etc
    linkDirs.push_back(d + "/$(Configuration)");
  }
  linkDirs.push_back("%(AdditionalLibraryDirectories)");
  linkOptions.AddFlag("AdditionalLibraryDirectories", linkDirs);

  std::string targetName;
  std::string targetNameSO;
  std::string targetNameFull;
  std::string targetNameImport;
  std::string targetNamePDB;
  if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE) {
    this->GeneratorTarget->GetExecutableNames(
      targetName, targetNameFull, targetNameImport, targetNamePDB, config);
  } else {
    this->GeneratorTarget->GetLibraryNames(targetName, targetNameSO,
                                           targetNameFull, targetNameImport,
                                           targetNamePDB, config);
  }

  if (this->MSTools) {
    if (this->GeneratorTarget->GetPropertyAsBool("WIN32_EXECUTABLE")) {
      if (this->GlobalGenerator->TargetsWindowsCE()) {
        linkOptions.AddFlag("SubSystem", "WindowsCE");
        if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE) {
          if (this->ClOptions[config]->UsingUnicode()) {
            linkOptions.AddFlag("EntryPointSymbol", "wWinMainCRTStartup");
          } else {
            linkOptions.AddFlag("EntryPointSymbol", "WinMainCRTStartup");
          }
        }
      } else {
        linkOptions.AddFlag("SubSystem", "Windows");
      }
    } else {
      if (this->GlobalGenerator->TargetsWindowsCE()) {
        linkOptions.AddFlag("SubSystem", "WindowsCE");
        if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE) {
          if (this->ClOptions[config]->UsingUnicode()) {
            linkOptions.AddFlag("EntryPointSymbol", "mainWCRTStartup");
          } else {
            linkOptions.AddFlag("EntryPointSymbol", "mainACRTStartup");
          }
        }
      } else {
        linkOptions.AddFlag("SubSystem", "Console");
      };
    }

    if (const char* stackVal = this->Makefile->GetDefinition(
          "CMAKE_" + linkLanguage + "_STACK_SIZE")) {
      linkOptions.AddFlag("StackReserveSize", stackVal);
    }

    linkOptions.AddFlag("GenerateDebugInformation", "false");

    std::string pdb = this->GeneratorTarget->GetPDBDirectory(config);
    pdb += "/";
    pdb += targetNamePDB;
    std::string imLib = this->GeneratorTarget->GetDirectory(
      config, cmStateEnums::ImportLibraryArtifact);
    imLib += "/";
    imLib += targetNameImport;

    linkOptions.AddFlag("ImportLibrary", imLib);
    linkOptions.AddFlag("ProgramDataBaseFile", pdb);

    // A Windows Runtime component uses internal .NET metadata,
    // so does not have an import library.
    if (this->GeneratorTarget->GetPropertyAsBool("VS_WINRT_COMPONENT") &&
        this->GeneratorTarget->GetType() != cmStateEnums::EXECUTABLE) {
      linkOptions.AddFlag("GenerateWindowsMetadata", "true");
    } else if (this->GlobalGenerator->TargetsWindowsPhone() ||
               this->GlobalGenerator->TargetsWindowsStore()) {
      // WindowsPhone and WindowsStore components are in an app container
      // and produce WindowsMetadata.  If we are not producing a WINRT
      // component, then do not generate the metadata here.
      linkOptions.AddFlag("GenerateWindowsMetadata", "false");
    }

    if (this->GlobalGenerator->TargetsWindowsPhone() &&
        this->GlobalGenerator->GetSystemVersion() == "8.0") {
      // WindowsPhone 8.0 does not have ole32.
      linkOptions.AppendFlag("IgnoreSpecificDefaultLibraries", "ole32.lib");
    }
  } else if (this->NsightTegra) {
    linkOptions.AddFlag("SoName", targetNameSO);
  }

  linkOptions.Parse(flags.c_str());
  linkOptions.FixManifestUACFlags();

  if (this->MSTools) {
    cmGeneratorTarget::ModuleDefinitionInfo const* mdi =
      this->GeneratorTarget->GetModuleDefinitionInfo(config);
    if (mdi && !mdi->DefFile.empty()) {
      linkOptions.AddFlag("ModuleDefinitionFile", mdi->DefFile.c_str());
    }
    linkOptions.AppendFlag("IgnoreSpecificDefaultLibraries",
                           "%(IgnoreSpecificDefaultLibraries)");
  }

  // VS 2015 without all updates has a v140 toolset whose
  // GenerateDebugInformation expects No/Debug instead of false/true.
  if (gg->GetPlatformToolsetNeedsDebugEnum()) {
    if (const char* debug = linkOptions.GetFlag("GenerateDebugInformation")) {
      if (strcmp(debug, "false") == 0) {
        linkOptions.AddFlag("GenerateDebugInformation", "No");
      } else if (strcmp(debug, "true") == 0) {
        linkOptions.AddFlag("GenerateDebugInformation", "Debug");
      }
    }
  }

  this->LinkOptions[config] = std::move(pOptions);
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeLibOptions()
{
  if (this->GeneratorTarget->GetType() == cmStateEnums::STATIC_LIBRARY) {
    for (std::string const& i : this->Configurations) {
      if (!this->ComputeLibOptions(i)) {
        return false;
      }
    }
  }
  return true;
}

bool cmVisualStudio10TargetGenerator::ComputeLibOptions(
  std::string const& config)
{
  cmComputeLinkInformation* pcli =
    this->GeneratorTarget->GetLinkInformation(config);
  if (!pcli) {
    cmSystemTools::Error(
      "CMake can not compute cmComputeLinkInformation for target: ",
      this->Name.c_str());
    return false;
  }

  cmComputeLinkInformation& cli = *pcli;
  typedef cmComputeLinkInformation::ItemVector ItemVector;
  const ItemVector& libs = cli.GetItems();
  std::string currentBinDir =
    this->LocalGenerator->GetCurrentBinaryDirectory();
  for (cmComputeLinkInformation::Item const& l : libs) {
    if (l.IsPath && cmVS10IsTargetsFile(l.Value)) {
      std::string path =
        this->LocalGenerator->ConvertToRelativePath(currentBinDir, l.Value);
      ConvertToWindowsSlash(path);
      this->AddTargetsFileAndConfigPair(path, config);
    }
  }

  return true;
}

void cmVisualStudio10TargetGenerator::WriteLinkOptions(
  std::string const& config)
{
  if (this->GeneratorTarget->GetType() == cmStateEnums::STATIC_LIBRARY ||
      this->GeneratorTarget->GetType() > cmStateEnums::MODULE_LIBRARY) {
    return;
  }
  if (this->ProjectType == csproj) {
    return;
  }
  Options& linkOptions = *(this->LinkOptions[config]);
  this->WriteString("<Link>\n", 2);

  linkOptions.PrependInheritedString("AdditionalOptions");
  linkOptions.OutputFlagMap(*this->BuildFileStream, "      ");

  this->WriteString("</Link>\n", 2);
  if (!this->GlobalGenerator->NeedLinkLibraryDependencies(
        this->GeneratorTarget)) {
    this->WriteString("<ProjectReference>\n", 2);
    this->WriteString(
      "<LinkLibraryDependencies>false</LinkLibraryDependencies>\n", 3);
    this->WriteString("</ProjectReference>\n", 2);
  }
}

void cmVisualStudio10TargetGenerator::AddLibraries(
  cmComputeLinkInformation& cli, std::vector<std::string>& libVec,
  std::vector<std::string>& vsTargetVec)
{
  typedef cmComputeLinkInformation::ItemVector ItemVector;
  ItemVector const& libs = cli.GetItems();
  std::string currentBinDir =
    this->LocalGenerator->GetCurrentBinaryDirectory();
  for (cmComputeLinkInformation::Item const& l : libs) {
    // Do not allow C# targets to be added to the LIB listing. LIB files are
    // used for linking C++ dependencies. C# libraries do not have lib files.
    // Instead, they compile down to C# reference libraries (DLL files). The
    // `<ProjectReference>` elements added to the vcxproj are enough for the
    // IDE to deduce the DLL file required by other C# projects that need its
    // reference library.
    if (l.Target &&
        cmGlobalVisualStudioGenerator::TargetIsCSharpOnly(l.Target)) {
      continue;
    }

    if (l.IsPath) {
      std::string path =
        this->LocalGenerator->ConvertToRelativePath(currentBinDir, l.Value);
      ConvertToWindowsSlash(path);
      if (cmVS10IsTargetsFile(l.Value)) {
        vsTargetVec.push_back(path);
      } else {
        libVec.push_back(path);
      }
    } else if (!l.Target ||
               l.Target->GetType() != cmStateEnums::INTERFACE_LIBRARY) {
      libVec.push_back(l.Value);
    }
  }
}

void cmVisualStudio10TargetGenerator::AddTargetsFileAndConfigPair(
  std::string const& targetsFile, std::string const& config)
{
  for (TargetsFileAndConfigs& i : this->TargetsFileAndConfigsVec) {
    if (cmSystemTools::ComparePath(targetsFile, i.File)) {
      if (std::find(i.Configs.begin(), i.Configs.end(), config) ==
          i.Configs.end()) {
        i.Configs.push_back(config);
      }
      return;
    }
  }
  TargetsFileAndConfigs entry;
  entry.File = targetsFile;
  entry.Configs.push_back(config);
  this->TargetsFileAndConfigsVec.push_back(entry);
}

void cmVisualStudio10TargetGenerator::WriteMidlOptions(
  std::string const& configName)
{
  if (!this->MSTools) {
    return;
  }
  if (this->ProjectType == csproj) {
    return;
  }

  // This processes *any* of the .idl files specified in the project's file
  // list (and passed as the item metadata %(Filename) expressing the rule
  // input filename) into output files at the per-config *build* dir
  // ($(IntDir)) each.
  //
  // IOW, this MIDL section is intended to provide a fully generic syntax
  // content suitable for most cases (read: if you get errors, then it's quite
  // probable that the error is on your side of the .idl setup).
  //
  // Also, note that the marked-as-generated _i.c file in the Visual Studio
  // generator case needs to be referred to as $(IntDir)\foo_i.c at the
  // project's file list, otherwise the compiler-side processing won't pick it
  // up (for non-directory form, it ends up looking in project binary dir
  // only).  Perhaps there's something to be done to make this more automatic
  // on the CMake side?
  this->WriteString("<Midl>\n", 2);
  this->WriteString("<AdditionalIncludeDirectories>", 3);
  std::vector<std::string> const includes =
    this->GetIncludes(configName, "MIDL");
  for (std::string const& i : includes) {
    *this->BuildFileStream << cmVS10EscapeXML(i) << ";";
  }
  this->WriteString("%(AdditionalIncludeDirectories)"
                    "</AdditionalIncludeDirectories>\n",
                    0);
  this->WriteString("<OutputDirectory>$(ProjectDir)/$(IntDir)"
                    "</OutputDirectory>\n",
                    3);
  this->WriteString("<HeaderFileName>%(Filename).h</HeaderFileName>\n", 3);
  this->WriteString("<TypeLibraryName>%(Filename).tlb</TypeLibraryName>\n", 3);
  this->WriteString("<InterfaceIdentifierFileName>"
                    "%(Filename)_i.c</InterfaceIdentifierFileName>\n",
                    3);
  this->WriteString("<ProxyFileName>%(Filename)_p.c</ProxyFileName>\n", 3);
  this->WriteString("</Midl>\n", 2);
}

void cmVisualStudio10TargetGenerator::WriteItemDefinitionGroups()
{
  if (this->ProjectType == csproj) {
    return;
  }
  for (const auto& i : this->Configurations) {
    this->WritePlatformConfigTag("ItemDefinitionGroup", i, 1);
    *this->BuildFileStream << "\n";
    //    output cl compile flags <ClCompile></ClCompile>
    if (this->GeneratorTarget->GetType() <= cmStateEnums::OBJECT_LIBRARY) {
      this->WriteClOptions(i);
      //    output rc compile flags <ResourceCompile></ResourceCompile>
      this->WriteRCOptions(i);
      this->WriteCudaOptions(i);
      this->WriteMasmOptions(i);
      this->WriteNasmOptions(i);
    }
    //    output midl flags       <Midl></Midl>
    this->WriteMidlOptions(i);
    // write events
    if (this->ProjectType != csproj) {
      this->WriteEvents(i);
    }
    //    output link flags       <Link></Link>
    this->WriteLinkOptions(i);
    this->WriteCudaLinkOptions(i);
    //    output lib flags       <Lib></Lib>
    this->WriteLibOptions(i);
    //    output manifest flags  <Manifest></Manifest>
    this->WriteManifestOptions(i);
    if (this->NsightTegra &&
        this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE &&
        this->GeneratorTarget->GetPropertyAsBool("ANDROID_GUI")) {
      this->WriteAntBuildOptions(i);
    }
    this->WriteString("</ItemDefinitionGroup>\n", 1);
  }
}

void cmVisualStudio10TargetGenerator::WriteEvents(
  std::string const& configName)
{
  bool addedPrelink = false;
  cmGeneratorTarget::ModuleDefinitionInfo const* mdi =
    this->GeneratorTarget->GetModuleDefinitionInfo(configName);
  if (mdi && mdi->DefFileGenerated) {
    addedPrelink = true;
    std::vector<cmCustomCommand> commands =
      this->GeneratorTarget->GetPreLinkCommands();
    this->GlobalGenerator->AddSymbolExportCommand(this->GeneratorTarget,
                                                  commands, configName);
    this->WriteEvent("PreLinkEvent", commands, configName);
  }
  if (!addedPrelink) {
    this->WriteEvent("PreLinkEvent",
                     this->GeneratorTarget->GetPreLinkCommands(), configName);
  }
  this->WriteEvent("PreBuildEvent",
                   this->GeneratorTarget->GetPreBuildCommands(), configName);
  this->WriteEvent("PostBuildEvent",
                   this->GeneratorTarget->GetPostBuildCommands(), configName);
}

void cmVisualStudio10TargetGenerator::WriteEvent(
  const char* name, std::vector<cmCustomCommand> const& commands,
  std::string const& configName)
{
  if (commands.empty()) {
    return;
  }
  this->WriteString("<", 2);
  (*this->BuildFileStream) << name << ">\n";
  cmLocalVisualStudio7Generator* lg = this->LocalGenerator;
  std::string script;
  const char* pre = "";
  std::string comment;
  for (cmCustomCommand const& i : commands) {
    cmCustomCommandGenerator ccg(i, configName, this->LocalGenerator);
    if (!ccg.HasOnlyEmptyCommandLines()) {
      comment += pre;
      comment += lg->ConstructComment(ccg);
      script += pre;
      pre = "\n";
      script += cmVS10EscapeXML(lg->ConstructScript(ccg));
    }
  }
  comment = cmVS10EscapeComment(comment);
  if (this->ProjectType != csproj) {
    this->WriteString("<Message>", 3);
    (*this->BuildFileStream) << cmVS10EscapeXML(comment) << "</Message>\n";
    this->WriteString("<Command>", 3);
  } else {
    std::string strippedComment = comment;
    strippedComment.erase(
      std::remove(strippedComment.begin(), strippedComment.end(), '\t'),
      strippedComment.end());
    if (!comment.empty() && !strippedComment.empty()) {
      (*this->BuildFileStream) << "echo " << cmVS10EscapeXML(comment) << "\n";
    }
  }
  (*this->BuildFileStream) << script;
  if (this->ProjectType != csproj) {
    (*this->BuildFileStream) << "</Command>";
  }
  (*this->BuildFileStream) << "\n";
  this->WriteString("</", 2);
  (*this->BuildFileStream) << name << ">\n";
}

void cmVisualStudio10TargetGenerator::WriteProjectReferences()
{
  cmGlobalGenerator::TargetDependSet const& unordered =
    this->GlobalGenerator->GetTargetDirectDepends(this->GeneratorTarget);
  typedef cmGlobalVisualStudioGenerator::OrderedTargetDependSet
    OrderedTargetDependSet;
  OrderedTargetDependSet depends(unordered, CMAKE_CHECK_BUILD_SYSTEM_TARGET);
  this->WriteString("<ItemGroup>\n", 1);
  for (cmTargetDepend const& i : depends) {
    cmGeneratorTarget const* dt = i;
    if (dt->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
      continue;
    }
    // skip fortran targets as they can not be processed by MSBuild
    // the only reference will be in the .sln file
    if (this->GlobalGenerator->TargetIsFortranOnly(dt)) {
      continue;
    }
    this->WriteString("<ProjectReference Include=\"", 2);
    cmLocalGenerator* lg = dt->GetLocalGenerator();
    std::string name = dt->GetName();
    std::string path;
    const char* p = dt->GetProperty("EXTERNAL_MSPROJECT");
    if (p) {
      path = p;
    } else {
      path = lg->GetCurrentBinaryDirectory();
      path += "/";
      path += dt->GetName();
      path += computeProjectFileExtension(dt, *this->Configurations.begin());
    }
    ConvertToWindowsSlash(path);
    (*this->BuildFileStream) << cmVS10EscapeXML(path) << "\">\n";
    this->WriteString("<Project>", 3);
    (*this->BuildFileStream) << "{" << this->GlobalGenerator->GetGUID(name)
                             << "}";
    (*this->BuildFileStream) << "</Project>\n";
    this->WriteString("<Name>", 3);
    (*this->BuildFileStream) << name << "</Name>\n";
    this->WriteDotNetReferenceCustomTags(name);
    if (csproj == this->ProjectType) {
      if (!this->GlobalGenerator->TargetCanBeReferenced(dt)) {
        this->WriteString(
          "<ReferenceOutputAssembly>false</ReferenceOutputAssembly>\n", 3);
      }
    }
    this->WriteString("</ProjectReference>\n", 2);
  }
  this->WriteString("</ItemGroup>\n", 1);
}

void cmVisualStudio10TargetGenerator::WritePlatformExtensions()
{
  // This only applies to Windows 10 apps
  if (this->GlobalGenerator->TargetsWindowsStore() &&
      cmHasLiteralPrefix(this->GlobalGenerator->GetSystemVersion(), "10.0")) {
    const char* desktopExtensionsVersion =
      this->GeneratorTarget->GetProperty("VS_DESKTOP_EXTENSIONS_VERSION");
    if (desktopExtensionsVersion) {
      this->WriteSinglePlatformExtension("WindowsDesktop",
                                         desktopExtensionsVersion);
    }
    const char* mobileExtensionsVersion =
      this->GeneratorTarget->GetProperty("VS_MOBILE_EXTENSIONS_VERSION");
    if (mobileExtensionsVersion) {
      this->WriteSinglePlatformExtension("WindowsMobile",
                                         mobileExtensionsVersion);
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteSinglePlatformExtension(
  std::string const& extension, std::string const& version)
{
  this->WriteString("<Import Project=", 2);
  (*this->BuildFileStream)
    << "\"$([Microsoft.Build.Utilities.ToolLocationHelper]"
    << "::GetPlatformExtensionSDKLocation(`" << extension
    << ", Version=" << version
    << "`, $(TargetPlatformIdentifier), $(TargetPlatformVersion), null, "
    << "$(ExtensionSDKDirectoryRoot), null))"
    << "\\DesignTime\\CommonConfiguration\\Neutral\\" << extension
    << ".props\" "
    << "Condition=\"exists('$("
    << "[Microsoft.Build.Utilities.ToolLocationHelper]"
    << "::GetPlatformExtensionSDKLocation(`" << extension
    << ", Version=" << version
    << "`, $(TargetPlatformIdentifier), $(TargetPlatformVersion), null, "
    << "$(ExtensionSDKDirectoryRoot), null))"
    << "\\DesignTime\\CommonConfiguration\\Neutral\\" << extension
    << ".props')\" />\n";
}

void cmVisualStudio10TargetGenerator::WriteSDKReferences()
{
  std::vector<std::string> sdkReferences;
  bool hasWrittenItemGroup = false;
  if (const char* vsSDKReferences =
        this->GeneratorTarget->GetProperty("VS_SDK_REFERENCES")) {
    cmSystemTools::ExpandListArgument(vsSDKReferences, sdkReferences);
    this->WriteString("<ItemGroup>\n", 1);
    hasWrittenItemGroup = true;
    for (std::string const& ri : sdkReferences) {
      this->WriteString("<SDKReference Include=\"", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(ri) << "\"/>\n";
    }
  }

  // This only applies to Windows 10 apps
  if (this->GlobalGenerator->TargetsWindowsStore() &&
      cmHasLiteralPrefix(this->GlobalGenerator->GetSystemVersion(), "10.0")) {
    const char* desktopExtensionsVersion =
      this->GeneratorTarget->GetProperty("VS_DESKTOP_EXTENSIONS_VERSION");
    const char* mobileExtensionsVersion =
      this->GeneratorTarget->GetProperty("VS_MOBILE_EXTENSIONS_VERSION");
    const char* iotExtensionsVersion =
      this->GeneratorTarget->GetProperty("VS_IOT_EXTENSIONS_VERSION");

    if (desktopExtensionsVersion || mobileExtensionsVersion ||
        iotExtensionsVersion) {
      if (!hasWrittenItemGroup) {
        this->WriteString("<ItemGroup>\n", 1);
        hasWrittenItemGroup = true;
      }
      if (desktopExtensionsVersion) {
        this->WriteSingleSDKReference("WindowsDesktop",
                                      desktopExtensionsVersion);
      }
      if (mobileExtensionsVersion) {
        this->WriteSingleSDKReference("WindowsMobile",
                                      mobileExtensionsVersion);
      }
      if (iotExtensionsVersion) {
        this->WriteSingleSDKReference("WindowsIoT", iotExtensionsVersion);
      }
    }
  }

  if (hasWrittenItemGroup) {
    this->WriteString("</ItemGroup>\n", 1);
  }
}

void cmVisualStudio10TargetGenerator::WriteSingleSDKReference(
  std::string const& extension, std::string const& version)
{
  this->WriteString("<SDKReference Include=\"", 2);
  (*this->BuildFileStream) << extension << ", Version=" << version
                           << "\" />\n";
}

void cmVisualStudio10TargetGenerator::WriteWinRTPackageCertificateKeyFile()
{
  if ((this->GlobalGenerator->TargetsWindowsStore() ||
       this->GlobalGenerator->TargetsWindowsPhone()) &&
      (cmStateEnums::EXECUTABLE == this->GeneratorTarget->GetType())) {
    std::string pfxFile;
    std::vector<cmSourceFile const*> certificates;
    this->GeneratorTarget->GetCertificates(certificates, "");
    for (cmSourceFile const* si : certificates) {
      pfxFile = this->ConvertPath(si->GetFullPath(), false);
      ConvertToWindowsSlash(pfxFile);
      break;
    }

    if (this->IsMissingFiles &&
        !(this->GlobalGenerator->TargetsWindowsPhone() &&
          this->GlobalGenerator->GetSystemVersion() == "8.0")) {
      // Move the manifest to a project directory to avoid clashes
      std::string artifactDir =
        this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
      ConvertToWindowsSlash(artifactDir);
      this->WriteString("<PropertyGroup>\n", 1);
      this->WriteString("<AppxPackageArtifactsDir>", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(artifactDir)
                               << "\\</AppxPackageArtifactsDir>\n";
      this->WriteString("<ProjectPriFullPath>", 2);
      std::string resourcePriFile =
        this->DefaultArtifactDir + "/resources.pri";
      ConvertToWindowsSlash(resourcePriFile);
      (*this->BuildFileStream) << resourcePriFile << "</ProjectPriFullPath>\n";

      // If we are missing files and we don't have a certificate and
      // aren't targeting WP8.0, add a default certificate
      if (pfxFile.empty()) {
        std::string templateFolder =
          cmSystemTools::GetCMakeRoot() + "/Templates/Windows";
        pfxFile = this->DefaultArtifactDir + "/Windows_TemporaryKey.pfx";
        cmSystemTools::CopyAFile(templateFolder + "/Windows_TemporaryKey.pfx",
                                 pfxFile, false);
        ConvertToWindowsSlash(pfxFile);
        this->AddedFiles.push_back(pfxFile);
      }

      this->WriteString("<", 2);
      (*this->BuildFileStream) << "PackageCertificateKeyFile>" << pfxFile
                               << "</PackageCertificateKeyFile>\n";
      std::string thumb = cmSystemTools::ComputeCertificateThumbprint(pfxFile);
      if (!thumb.empty()) {
        this->WriteString("<PackageCertificateThumbprint>", 2);
        (*this->BuildFileStream) << thumb
                                 << "</PackageCertificateThumbprint>\n";
      }
      this->WriteString("</PropertyGroup>\n", 1);
    } else if (!pfxFile.empty()) {
      this->WriteString("<PropertyGroup>\n", 1);
      this->WriteString("<", 2);
      (*this->BuildFileStream) << "PackageCertificateKeyFile>" << pfxFile
                               << "</PackageCertificateKeyFile>\n";
      std::string thumb = cmSystemTools::ComputeCertificateThumbprint(pfxFile);
      if (!thumb.empty()) {
        this->WriteString("<PackageCertificateThumbprint>", 2);
        (*this->BuildFileStream) << thumb
                                 << "</PackageCertificateThumbprint>\n";
      }
      this->WriteString("</PropertyGroup>\n", 1);
    }
  }
}

bool cmVisualStudio10TargetGenerator::IsResxHeader(
  const std::string& headerFile)
{
  std::set<std::string> expectedResxHeaders;
  this->GeneratorTarget->GetExpectedResxHeaders(expectedResxHeaders, "");

  std::set<std::string>::const_iterator it =
    expectedResxHeaders.find(headerFile);
  return it != expectedResxHeaders.end();
}

bool cmVisualStudio10TargetGenerator::IsXamlHeader(
  const std::string& headerFile)
{
  std::set<std::string> expectedXamlHeaders;
  this->GeneratorTarget->GetExpectedXamlHeaders(expectedXamlHeaders, "");

  std::set<std::string>::const_iterator it =
    expectedXamlHeaders.find(headerFile);
  return it != expectedXamlHeaders.end();
}

bool cmVisualStudio10TargetGenerator::IsXamlSource(
  const std::string& sourceFile)
{
  std::set<std::string> expectedXamlSources;
  this->GeneratorTarget->GetExpectedXamlSources(expectedXamlSources, "");

  std::set<std::string>::const_iterator it =
    expectedXamlSources.find(sourceFile);
  return it != expectedXamlSources.end();
}

void cmVisualStudio10TargetGenerator::WriteApplicationTypeSettings()
{
  cmGlobalVisualStudio10Generator* gg = this->GlobalGenerator;
  bool isAppContainer = false;
  bool const isWindowsPhone = this->GlobalGenerator->TargetsWindowsPhone();
  bool const isWindowsStore = this->GlobalGenerator->TargetsWindowsStore();
  std::string const& v = this->GlobalGenerator->GetSystemVersion();
  if (isWindowsPhone || isWindowsStore) {
    this->WriteString("<ApplicationType>", 2);
    (*this->BuildFileStream)
      << (isWindowsPhone ? "Windows Phone" : "Windows Store")
      << "</ApplicationType>\n";
    this->WriteString("<DefaultLanguage>en-US"
                      "</DefaultLanguage>\n",
                      2);
    if (cmHasLiteralPrefix(v, "10.0")) {
      this->WriteString("<ApplicationTypeRevision>", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML("10.0")
                               << "</ApplicationTypeRevision>\n";
      // Visual Studio 14.0 is necessary for building 10.0 apps
      this->WriteString("<MinimumVisualStudioVersion>14.0"
                        "</MinimumVisualStudioVersion>\n",
                        2);

      if (this->GeneratorTarget->GetType() < cmStateEnums::UTILITY) {
        isAppContainer = true;
      }
    } else if (v == "8.1") {
      this->WriteString("<ApplicationTypeRevision>", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(v)
                               << "</ApplicationTypeRevision>\n";
      // Visual Studio 12.0 is necessary for building 8.1 apps
      this->WriteString("<MinimumVisualStudioVersion>12.0"
                        "</MinimumVisualStudioVersion>\n",
                        2);

      if (this->GeneratorTarget->GetType() < cmStateEnums::UTILITY) {
        isAppContainer = true;
      }
    } else if (v == "8.0") {
      this->WriteString("<ApplicationTypeRevision>", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(v)
                               << "</ApplicationTypeRevision>\n";
      // Visual Studio 11.0 is necessary for building 8.0 apps
      this->WriteString("<MinimumVisualStudioVersion>11.0"
                        "</MinimumVisualStudioVersion>\n",
                        2);

      if (isWindowsStore &&
          this->GeneratorTarget->GetType() < cmStateEnums::UTILITY) {
        isAppContainer = true;
      } else if (isWindowsPhone &&
                 this->GeneratorTarget->GetType() ==
                   cmStateEnums::EXECUTABLE) {
        this->WriteString("<XapOutputs>true</XapOutputs>\n", 2);
        this->WriteString("<XapFilename>", 2);
        (*this->BuildFileStream)
          << cmVS10EscapeXML(this->Name)
          << "_$(Configuration)_$(Platform).xap</XapFilename>\n";
      }
    }
  }
  if (isAppContainer) {
    this->WriteString("<AppContainerApplication>true"
                      "</AppContainerApplication>\n",
                      2);
  } else if (this->Platform == "ARM64") {
    this->WriteString("<WindowsSDKDesktopARM64Support>true"
                      "</WindowsSDKDesktopARM64Support>\n",
                      2);
  } else if (this->Platform == "ARM") {
    this->WriteString("<WindowsSDKDesktopARMSupport>true"
                      "</WindowsSDKDesktopARMSupport>\n",
                      2);
  }
  std::string const& targetPlatformVersion =
    gg->GetWindowsTargetPlatformVersion();
  if (!targetPlatformVersion.empty()) {
    this->WriteString("<WindowsTargetPlatformVersion>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(targetPlatformVersion)
                             << "</WindowsTargetPlatformVersion>\n";
  }
  const char* targetPlatformMinVersion = this->GeneratorTarget->GetProperty(
    "VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION");
  if (targetPlatformMinVersion) {
    this->WriteString("<WindowsTargetPlatformMinVersion>", 2);
    (*this->BuildFileStream) << cmVS10EscapeXML(targetPlatformMinVersion)
                             << "</WindowsTargetPlatformMinVersion>\n";
  } else if (isWindowsStore && cmHasLiteralPrefix(v, "10.0")) {
    // If the min version is not set, then use the TargetPlatformVersion
    if (!targetPlatformVersion.empty()) {
      this->WriteString("<WindowsTargetPlatformMinVersion>", 2);
      (*this->BuildFileStream) << cmVS10EscapeXML(targetPlatformVersion)
                               << "</WindowsTargetPlatformMinVersion>\n";
    }
  }

  // Added IoT Startup Task support
  if (this->GeneratorTarget->GetPropertyAsBool("VS_IOT_STARTUP_TASK")) {
    this->WriteString("<ContainsStartupTask>true</ContainsStartupTask>\n", 2);
  }
}

void cmVisualStudio10TargetGenerator::VerifyNecessaryFiles()
{
  // For Windows and Windows Phone executables, we will assume that if a
  // manifest is not present that we need to add all the necessary files
  if (this->GeneratorTarget->GetType() == cmStateEnums::EXECUTABLE) {
    std::vector<cmSourceFile const*> manifestSources;
    this->GeneratorTarget->GetAppManifest(manifestSources, "");
    {
      std::string const& v = this->GlobalGenerator->GetSystemVersion();
      if (this->GlobalGenerator->TargetsWindowsPhone()) {
        if (v == "8.0") {
          // Look through the sources for WMAppManifest.xml
          std::vector<cmSourceFile const*> extraSources;
          this->GeneratorTarget->GetExtraSources(extraSources, "");
          bool foundManifest = false;
          for (cmSourceFile const* si : extraSources) {
            // Need to do a lowercase comparison on the filename
            if ("wmappmanifest.xml" ==
                cmSystemTools::LowerCase(si->GetLocation().GetName())) {
              foundManifest = true;
              break;
            }
          }
          if (!foundManifest) {
            this->IsMissingFiles = true;
          }
        } else if (v == "8.1") {
          if (manifestSources.empty()) {
            this->IsMissingFiles = true;
          }
        }
      } else if (this->GlobalGenerator->TargetsWindowsStore()) {
        if (manifestSources.empty()) {
          if (v == "8.0") {
            this->IsMissingFiles = true;
          } else if (v == "8.1" || cmHasLiteralPrefix(v, "10.0")) {
            this->IsMissingFiles = true;
          }
        }
      }
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteMissingFiles()
{
  std::string const& v = this->GlobalGenerator->GetSystemVersion();
  if (this->GlobalGenerator->TargetsWindowsPhone()) {
    if (v == "8.0") {
      this->WriteMissingFilesWP80();
    } else if (v == "8.1") {
      this->WriteMissingFilesWP81();
    }
  } else if (this->GlobalGenerator->TargetsWindowsStore()) {
    if (v == "8.0") {
      this->WriteMissingFilesWS80();
    } else if (v == "8.1") {
      this->WriteMissingFilesWS81();
    } else if (cmHasLiteralPrefix(v, "10.0")) {
      this->WriteMissingFilesWS10_0();
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteMissingFilesWP80()
{
  std::string templateFolder =
    cmSystemTools::GetCMakeRoot() + "/Templates/Windows";

  // For WP80, the manifest needs to be in the same folder as the project
  // this can cause an overwrite problem if projects aren't organized in
  // folders
  std::string manifestFile =
    this->LocalGenerator->GetCurrentBinaryDirectory() +
    std::string("/WMAppManifest.xml");
  std::string artifactDir =
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
  ConvertToWindowsSlash(artifactDir);
  std::string artifactDirXML = cmVS10EscapeXML(artifactDir);
  std::string targetNameXML =
    cmVS10EscapeXML(this->GeneratorTarget->GetName());

  cmGeneratedFileStream fout(manifestFile.c_str());
  fout.SetCopyIfDifferent(true);

  /* clang-format off */
  fout <<
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Deployment"
    " xmlns=\"http://schemas.microsoft.com/windowsphone/2012/deployment\""
    " AppPlatformVersion=\"8.0\">\n"
    "\t<DefaultLanguage xmlns=\"\" code=\"en-US\"/>\n"
    "\t<App xmlns=\"\" ProductID=\"{" << this->GUID << "}\""
    " Title=\"CMake Test Program\" RuntimeType=\"Modern Native\""
    " Version=\"1.0.0.0\" Genre=\"apps.normal\"  Author=\"CMake\""
    " Description=\"Default CMake App\" Publisher=\"CMake\""
    " PublisherID=\"{" << this->GUID << "}\">\n"
    "\t\t<IconPath IsRelative=\"true\" IsResource=\"false\">"
       << artifactDirXML << "\\ApplicationIcon.png</IconPath>\n"
    "\t\t<Capabilities/>\n"
    "\t\t<Tasks>\n"
    "\t\t\t<DefaultTask Name=\"_default\""
    " ImagePath=\"" << targetNameXML << ".exe\" ImageParams=\"\" />\n"
    "\t\t</Tasks>\n"
    "\t\t<Tokens>\n"
    "\t\t\t<PrimaryToken TokenID=\"" << targetNameXML << "Token\""
    " TaskName=\"_default\">\n"
    "\t\t\t\t<TemplateFlip>\n"
    "\t\t\t\t\t<SmallImageURI IsRelative=\"true\" IsResource=\"false\">"
       << artifactDirXML << "\\SmallLogo.png</SmallImageURI>\n"
    "\t\t\t\t\t<Count>0</Count>\n"
    "\t\t\t\t\t<BackgroundImageURI IsRelative=\"true\" IsResource=\"false\">"
       << artifactDirXML << "\\Logo.png</BackgroundImageURI>\n"
    "\t\t\t\t</TemplateFlip>\n"
    "\t\t\t</PrimaryToken>\n"
    "\t\t</Tokens>\n"
    "\t\t<ScreenResolutions>\n"
    "\t\t\t<ScreenResolution Name=\"ID_RESOLUTION_WVGA\" />\n"
    "\t\t</ScreenResolutions>\n"
    "\t</App>\n"
    "</Deployment>\n";
  /* clang-format on */

  std::string sourceFile = this->ConvertPath(manifestFile, false);
  ConvertToWindowsSlash(sourceFile);
  this->WriteString("<Xml Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(sourceFile) << "\">\n";
  this->WriteString("<SubType>Designer</SubType>\n", 3);
  this->WriteString("</Xml>\n", 2);
  this->AddedFiles.push_back(sourceFile);

  std::string smallLogo = this->DefaultArtifactDir + "/SmallLogo.png";
  cmSystemTools::CopyAFile(templateFolder + "/SmallLogo.png", smallLogo,
                           false);
  ConvertToWindowsSlash(smallLogo);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(smallLogo) << "\" />\n";
  this->AddedFiles.push_back(smallLogo);

  std::string logo = this->DefaultArtifactDir + "/Logo.png";
  cmSystemTools::CopyAFile(templateFolder + "/Logo.png", logo, false);
  ConvertToWindowsSlash(logo);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(logo) << "\" />\n";
  this->AddedFiles.push_back(logo);

  std::string applicationIcon =
    this->DefaultArtifactDir + "/ApplicationIcon.png";
  cmSystemTools::CopyAFile(templateFolder + "/ApplicationIcon.png",
                           applicationIcon, false);
  ConvertToWindowsSlash(applicationIcon);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(applicationIcon) << "\" />\n";
  this->AddedFiles.push_back(applicationIcon);
}

void cmVisualStudio10TargetGenerator::WriteMissingFilesWP81()
{
  std::string manifestFile =
    this->DefaultArtifactDir + "/package.appxManifest";
  std::string artifactDir =
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
  ConvertToWindowsSlash(artifactDir);
  std::string artifactDirXML = cmVS10EscapeXML(artifactDir);
  std::string targetNameXML =
    cmVS10EscapeXML(this->GeneratorTarget->GetName());

  cmGeneratedFileStream fout(manifestFile.c_str());
  fout.SetCopyIfDifferent(true);

  /* clang-format off */
  fout <<
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\""
    " xmlns:m2=\"http://schemas.microsoft.com/appx/2013/manifest\""
    " xmlns:mp=\"http://schemas.microsoft.com/appx/2014/phone/manifest\">\n"
    "\t<Identity Name=\"" << this->GUID << "\" Publisher=\"CN=CMake\""
    " Version=\"1.0.0.0\" />\n"
    "\t<mp:PhoneIdentity PhoneProductId=\"" << this->GUID << "\""
    " PhonePublisherId=\"00000000-0000-0000-0000-000000000000\"/>\n"
    "\t<Properties>\n"
    "\t\t<DisplayName>" << targetNameXML << "</DisplayName>\n"
    "\t\t<PublisherDisplayName>CMake</PublisherDisplayName>\n"
    "\t\t<Logo>" << artifactDirXML << "\\StoreLogo.png</Logo>\n"
    "\t</Properties>\n"
    "\t<Prerequisites>\n"
    "\t\t<OSMinVersion>6.3.1</OSMinVersion>\n"
    "\t\t<OSMaxVersionTested>6.3.1</OSMaxVersionTested>\n"
    "\t</Prerequisites>\n"
    "\t<Resources>\n"
    "\t\t<Resource Language=\"x-generate\" />\n"
    "\t</Resources>\n"
    "\t<Applications>\n"
    "\t\t<Application Id=\"App\""
    " Executable=\"" << targetNameXML << ".exe\""
    " EntryPoint=\"" << targetNameXML << ".App\">\n"
    "\t\t\t<m2:VisualElements\n"
    "\t\t\t\tDisplayName=\"" << targetNameXML << "\"\n"
    "\t\t\t\tDescription=\"" << targetNameXML << "\"\n"
    "\t\t\t\tBackgroundColor=\"#336699\"\n"
    "\t\t\t\tForegroundText=\"light\"\n"
    "\t\t\t\tSquare150x150Logo=\"" << artifactDirXML << "\\Logo.png\"\n"
    "\t\t\t\tSquare30x30Logo=\"" << artifactDirXML << "\\SmallLogo.png\">\n"
    "\t\t\t\t<m2:DefaultTile ShortName=\"" << targetNameXML << "\">\n"
    "\t\t\t\t\t<m2:ShowNameOnTiles>\n"
    "\t\t\t\t\t\t<m2:ShowOn Tile=\"square150x150Logo\" />\n"
    "\t\t\t\t\t</m2:ShowNameOnTiles>\n"
    "\t\t\t\t</m2:DefaultTile>\n"
    "\t\t\t\t<m2:SplashScreen"
    " Image=\"" << artifactDirXML << "\\SplashScreen.png\" />\n"
    "\t\t\t</m2:VisualElements>\n"
    "\t\t</Application>\n"
    "\t</Applications>\n"
    "</Package>\n";
  /* clang-format on */

  this->WriteCommonMissingFiles(manifestFile);
}

void cmVisualStudio10TargetGenerator::WriteMissingFilesWS80()
{
  std::string manifestFile =
    this->DefaultArtifactDir + "/package.appxManifest";
  std::string artifactDir =
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
  ConvertToWindowsSlash(artifactDir);
  std::string artifactDirXML = cmVS10EscapeXML(artifactDir);
  std::string targetNameXML =
    cmVS10EscapeXML(this->GeneratorTarget->GetName());

  cmGeneratedFileStream fout(manifestFile.c_str());
  fout.SetCopyIfDifferent(true);

  /* clang-format off */
  fout <<
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\">\n"
    "\t<Identity Name=\"" << this->GUID << "\" Publisher=\"CN=CMake\""
    " Version=\"1.0.0.0\" />\n"
    "\t<Properties>\n"
    "\t\t<DisplayName>" << targetNameXML << "</DisplayName>\n"
    "\t\t<PublisherDisplayName>CMake</PublisherDisplayName>\n"
    "\t\t<Logo>" << artifactDirXML << "\\StoreLogo.png</Logo>\n"
    "\t</Properties>\n"
    "\t<Prerequisites>\n"
    "\t\t<OSMinVersion>6.2.1</OSMinVersion>\n"
    "\t\t<OSMaxVersionTested>6.2.1</OSMaxVersionTested>\n"
    "\t</Prerequisites>\n"
    "\t<Resources>\n"
    "\t\t<Resource Language=\"x-generate\" />\n"
    "\t</Resources>\n"
    "\t<Applications>\n"
    "\t\t<Application Id=\"App\""
    " Executable=\"" << targetNameXML << ".exe\""
    " EntryPoint=\"" << targetNameXML << ".App\">\n"
    "\t\t\t<VisualElements"
    " DisplayName=\"" << targetNameXML << "\""
    " Description=\"" << targetNameXML << "\""
    " BackgroundColor=\"#336699\" ForegroundText=\"light\""
    " Logo=\"" << artifactDirXML << "\\Logo.png\""
    " SmallLogo=\"" << artifactDirXML << "\\SmallLogo.png\">\n"
    "\t\t\t\t<DefaultTile ShowName=\"allLogos\""
    " ShortName=\"" << targetNameXML << "\" />\n"
    "\t\t\t\t<SplashScreen"
    " Image=\"" << artifactDirXML << "\\SplashScreen.png\" />\n"
    "\t\t\t</VisualElements>\n"
    "\t\t</Application>\n"
    "\t</Applications>\n"
    "</Package>\n";
  /* clang-format on */

  this->WriteCommonMissingFiles(manifestFile);
}

void cmVisualStudio10TargetGenerator::WriteMissingFilesWS81()
{
  std::string manifestFile =
    this->DefaultArtifactDir + "/package.appxManifest";
  std::string artifactDir =
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
  ConvertToWindowsSlash(artifactDir);
  std::string artifactDirXML = cmVS10EscapeXML(artifactDir);
  std::string targetNameXML =
    cmVS10EscapeXML(this->GeneratorTarget->GetName());

  cmGeneratedFileStream fout(manifestFile.c_str());
  fout.SetCopyIfDifferent(true);

  /* clang-format off */
  fout <<
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\""
    " xmlns:m2=\"http://schemas.microsoft.com/appx/2013/manifest\">\n"
    "\t<Identity Name=\"" << this->GUID << "\" Publisher=\"CN=CMake\""
    " Version=\"1.0.0.0\" />\n"
    "\t<Properties>\n"
    "\t\t<DisplayName>" << targetNameXML << "</DisplayName>\n"
    "\t\t<PublisherDisplayName>CMake</PublisherDisplayName>\n"
    "\t\t<Logo>" << artifactDirXML << "\\StoreLogo.png</Logo>\n"
    "\t</Properties>\n"
    "\t<Prerequisites>\n"
    "\t\t<OSMinVersion>6.3</OSMinVersion>\n"
    "\t\t<OSMaxVersionTested>6.3</OSMaxVersionTested>\n"
    "\t</Prerequisites>\n"
    "\t<Resources>\n"
    "\t\t<Resource Language=\"x-generate\" />\n"
    "\t</Resources>\n"
    "\t<Applications>\n"
    "\t\t<Application Id=\"App\""
    " Executable=\"" << targetNameXML << ".exe\""
    " EntryPoint=\"" << targetNameXML << ".App\">\n"
    "\t\t\t<m2:VisualElements\n"
    "\t\t\t\tDisplayName=\"" << targetNameXML << "\"\n"
    "\t\t\t\tDescription=\"" << targetNameXML << "\"\n"
    "\t\t\t\tBackgroundColor=\"#336699\"\n"
    "\t\t\t\tForegroundText=\"light\"\n"
    "\t\t\t\tSquare150x150Logo=\"" << artifactDirXML << "\\Logo.png\"\n"
    "\t\t\t\tSquare30x30Logo=\"" << artifactDirXML << "\\SmallLogo.png\">\n"
    "\t\t\t\t<m2:DefaultTile ShortName=\"" << targetNameXML << "\">\n"
    "\t\t\t\t\t<m2:ShowNameOnTiles>\n"
    "\t\t\t\t\t\t<m2:ShowOn Tile=\"square150x150Logo\" />\n"
    "\t\t\t\t\t</m2:ShowNameOnTiles>\n"
    "\t\t\t\t</m2:DefaultTile>\n"
    "\t\t\t\t<m2:SplashScreen"
    " Image=\"" << artifactDirXML << "\\SplashScreen.png\" />\n"
    "\t\t\t</m2:VisualElements>\n"
    "\t\t</Application>\n"
    "\t</Applications>\n"
    "</Package>\n";
  /* clang-format on */

  this->WriteCommonMissingFiles(manifestFile);
}

void cmVisualStudio10TargetGenerator::WriteMissingFilesWS10_0()
{
  std::string manifestFile =
    this->DefaultArtifactDir + "/package.appxManifest";
  std::string artifactDir =
    this->LocalGenerator->GetTargetDirectory(this->GeneratorTarget);
  ConvertToWindowsSlash(artifactDir);
  std::string artifactDirXML = cmVS10EscapeXML(artifactDir);
  std::string targetNameXML =
    cmVS10EscapeXML(this->GeneratorTarget->GetName());

  cmGeneratedFileStream fout(manifestFile.c_str());
  fout.SetCopyIfDifferent(true);

  /* clang-format off */
  fout <<
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Package\n\t"
    "xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
    "\txmlns:mp=\"http://schemas.microsoft.com/appx/2014/phone/manifest\"\n"
    "\txmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\""
    "\n\tIgnorableNamespaces=\"uap mp\">\n\n"
    "\t<Identity Name=\"" << this->GUID << "\" Publisher=\"CN=CMake\""
    " Version=\"1.0.0.0\" />\n"
    "\t<mp:PhoneIdentity PhoneProductId=\"" << this->GUID <<
    "\" PhonePublisherId=\"00000000-0000-0000-0000-000000000000\"/>\n"
    "\t<Properties>\n"
    "\t\t<DisplayName>" << targetNameXML << "</DisplayName>\n"
    "\t\t<PublisherDisplayName>CMake</PublisherDisplayName>\n"
    "\t\t<Logo>" << artifactDirXML << "\\StoreLogo.png</Logo>\n"
    "\t</Properties>\n"
    "\t<Dependencies>\n"
    "\t\t<TargetDeviceFamily Name=\"Windows.Universal\" "
    "MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\" />\n"
    "\t</Dependencies>\n"

    "\t<Resources>\n"
    "\t\t<Resource Language=\"x-generate\" />\n"
    "\t</Resources>\n"
    "\t<Applications>\n"
    "\t\t<Application Id=\"App\""
    " Executable=\"" << targetNameXML << ".exe\""
    " EntryPoint=\"" << targetNameXML << ".App\">\n"
    "\t\t\t<uap:VisualElements\n"
    "\t\t\t\tDisplayName=\"" << targetNameXML << "\"\n"
    "\t\t\t\tDescription=\"" << targetNameXML << "\"\n"
    "\t\t\t\tBackgroundColor=\"#336699\"\n"
    "\t\t\t\tSquare150x150Logo=\"" << artifactDirXML << "\\Logo.png\"\n"
    "\t\t\t\tSquare44x44Logo=\"" << artifactDirXML <<
    "\\SmallLogo44x44.png\">\n"
    "\t\t\t\t<uap:SplashScreen"
    " Image=\"" << artifactDirXML << "\\SplashScreen.png\" />\n"
    "\t\t\t</uap:VisualElements>\n"
    "\t\t</Application>\n"
    "\t</Applications>\n"
    "</Package>\n";
  /* clang-format on */

  this->WriteCommonMissingFiles(manifestFile);
}

void cmVisualStudio10TargetGenerator::WriteCommonMissingFiles(
  const std::string& manifestFile)
{
  std::string templateFolder =
    cmSystemTools::GetCMakeRoot() + "/Templates/Windows";

  std::string sourceFile = this->ConvertPath(manifestFile, false);
  ConvertToWindowsSlash(sourceFile);
  this->WriteString("<AppxManifest Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(sourceFile) << "\">\n";
  this->WriteString("<SubType>Designer</SubType>\n", 3);
  this->WriteString("</AppxManifest>\n", 2);
  this->AddedFiles.push_back(sourceFile);

  std::string smallLogo = this->DefaultArtifactDir + "/SmallLogo.png";
  cmSystemTools::CopyAFile(templateFolder + "/SmallLogo.png", smallLogo,
                           false);
  ConvertToWindowsSlash(smallLogo);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(smallLogo) << "\" />\n";
  this->AddedFiles.push_back(smallLogo);

  std::string smallLogo44 = this->DefaultArtifactDir + "/SmallLogo44x44.png";
  cmSystemTools::CopyAFile(templateFolder + "/SmallLogo44x44.png", smallLogo44,
                           false);
  ConvertToWindowsSlash(smallLogo44);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(smallLogo44) << "\" />\n";
  this->AddedFiles.push_back(smallLogo44);

  std::string logo = this->DefaultArtifactDir + "/Logo.png";
  cmSystemTools::CopyAFile(templateFolder + "/Logo.png", logo, false);
  ConvertToWindowsSlash(logo);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(logo) << "\" />\n";
  this->AddedFiles.push_back(logo);

  std::string storeLogo = this->DefaultArtifactDir + "/StoreLogo.png";
  cmSystemTools::CopyAFile(templateFolder + "/StoreLogo.png", storeLogo,
                           false);
  ConvertToWindowsSlash(storeLogo);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(storeLogo) << "\" />\n";
  this->AddedFiles.push_back(storeLogo);

  std::string splashScreen = this->DefaultArtifactDir + "/SplashScreen.png";
  cmSystemTools::CopyAFile(templateFolder + "/SplashScreen.png", splashScreen,
                           false);
  ConvertToWindowsSlash(splashScreen);
  this->WriteString("<Image Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(splashScreen) << "\" />\n";
  this->AddedFiles.push_back(splashScreen);

  // This file has already been added to the build so don't copy it
  std::string keyFile = this->DefaultArtifactDir + "/Windows_TemporaryKey.pfx";
  ConvertToWindowsSlash(keyFile);
  this->WriteString("<None Include=\"", 2);
  (*this->BuildFileStream) << cmVS10EscapeXML(keyFile) << "\" />\n";
}

bool cmVisualStudio10TargetGenerator::ForceOld(const std::string& source) const
{
  HANDLE h =
    CreateFileW(cmSystemTools::ConvertToWindowsExtendedPath(source).c_str(),
                FILE_WRITE_ATTRIBUTES, FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, 0);
  if (!h) {
    return false;
  }

  FILETIME const ftime_20010101 = { 3365781504u, 29389701u };
  if (!SetFileTime(h, &ftime_20010101, &ftime_20010101, &ftime_20010101)) {
    CloseHandle(h);
    return false;
  }

  CloseHandle(h);
  return true;
}

void cmVisualStudio10TargetGenerator::GetCSharpSourceProperties(
  cmSourceFile const* sf, std::map<std::string, std::string>& tags)
{
  if (this->ProjectType == csproj) {
    const cmPropertyMap& props = sf->GetProperties();
    for (auto const& p : props) {
      static const std::string propNamePrefix = "VS_CSHARP_";
      if (p.first.find(propNamePrefix) == 0) {
        std::string tagName = p.first.substr(propNamePrefix.length());
        if (!tagName.empty()) {
          const std::string val = props.GetPropertyValue(p.first);
          if (!val.empty()) {
            tags[tagName] = val;
          } else {
            tags.erase(tagName);
          }
        }
      }
    }
  }
}

void cmVisualStudio10TargetGenerator::WriteCSharpSourceProperties(
  const std::map<std::string, std::string>& tags)
{
  if (!tags.empty()) {
    for (const auto& i : tags) {
      this->WriteString("<", 3);
      (*this->BuildFileStream) << i.first << ">" << cmVS10EscapeXML(i.second)
                               << "</" << i.first << ">\n";
    }
  }
}

void cmVisualStudio10TargetGenerator::GetCSharpSourceLink(
  cmSourceFile const* sf, std::string& link)
{
  std::string f = sf->GetFullPath();
  if (!this->InSourceBuild) {
    const std::string stripFromPath =
      this->Makefile->GetCurrentSourceDirectory();
    if (f.find(stripFromPath) != std::string::npos) {
      link = f.substr(stripFromPath.length() + 1);
      if (const char* l = sf->GetProperty("VS_CSHARP_Link")) {
        link = l;
      }
      ConvertToWindowsSlash(link);
    }
  }
}

std::string cmVisualStudio10TargetGenerator::GetCMakeFilePath(
  const char* relativeFilePath) const
{
  // Always search in the standard modules location.
  std::string path = cmSystemTools::GetCMakeRoot() + "/";
  path += relativeFilePath;
  ConvertToWindowsSlash(path);

  return path;
}
