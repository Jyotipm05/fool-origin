#!/usr/bin/env python3
"""
build_vsix.py
Packages the Fool VS Code extension into a spec-compliant .vsix archive.
"""

import os
import zipfile
import json

def build_vsix():
    vscode_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(os.path.dirname(vscode_dir))
    
    with open(os.path.join(vscode_dir, "package.json"), "r", encoding="utf-8") as f:
        pkg = json.load(f)
        
    ext_id = pkg.get("name", "fool-vscode")
    version = pkg.get("version", "0.1.0")
    publisher = pkg.get("publisher", "fool-origin")
    display_name = pkg.get("displayName", "Fool Language Support")
    description = pkg.get("description", "Fool Language Support")
    
    vsix_filename = f"{ext_id}-{version}.vsix"
    output_path = os.path.join(vscode_dir, vsix_filename)
    root_output_path = os.path.join(project_root, vsix_filename)

    content_types_xml = """<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".json" ContentType="application/json"/>
  <Default Extension=".vsixmanifest" ContentType="text/xml"/>
  <Default Extension=".md" ContentType="text/markdown"/>
  <Default Extension=".txt" ContentType="text/plain"/>
</Types>
"""

    vsixmanifest_xml = f"""<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Id="{ext_id}" Version="{version}" Publisher="{publisher}" Language="en-US"/>
    <DisplayName>{display_name}</DisplayName>
    <Description xml:space="preserve">{description}</Description>
    <Tags>fool,systems programming,syntax,highlighting,snippets</Tags>
    <Categories>Programming Languages,Snippets</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.75.0"/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value=""/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value=""/>
      <Property Id="Microsoft.VisualStudio.Code.LocalizedLanguages" Value=""/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace"/>
    </Properties>
    <License>extension/LICENSE</License>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Content.License" Path="extension/LICENSE" Addressable="true"/>
  </Assets>
</PackageManifest>
"""

    files_to_pack = [
        "package.json",
        "language-configuration.json",
        "README.md",
        "LICENSE",
        os.path.join("syntaxes", "fool.tmLanguage.json"),
        os.path.join("snippets", "fool.json"),
    ]

    print(f"Creating VSIX archive: {vsix_filename}...")
    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # 1. Open Packaging Conventions descriptor
        zf.writestr("[Content_Types].xml", content_types_xml)
        
        # 2. Extension VSIX manifest
        zf.writestr("extension.vsixmanifest", vsixmanifest_xml)
        
        # 3. Extension files under 'extension/' prefix
        for rel_file in files_to_pack:
            abs_path = os.path.join(vscode_dir, rel_file)
            if os.path.exists(abs_path):
                archive_name = "extension/" + rel_file.replace("\\", "/")
                zf.write(abs_path, archive_name)
                print(f"  + Added: {archive_name}")
            else:
                print(f"  ! Missing file: {abs_path}")

    # Copy to project root for convenience
    import shutil
    shutil.copy2(output_path, root_output_path)
    print(f"\nVSIX successfully built:")
    print(f"  - {output_path}")
    print(f"  - {root_output_path}")

if __name__ == "__main__":
    build_vsix()

