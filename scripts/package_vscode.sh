#!/usr/bin/env bash
# Packages the VS Code extension in editors/vscode as a .vsix with plain `zip` (no Node needed).
# Usage: scripts/package_vscode.sh   ->  editors/vscode/rift-language-<version>.vsix
set -euo pipefail
src="$(cd "$(dirname "$0")/../editors/vscode" && pwd)"
version="$(sed -n 's/^  "version": "\(.*\)",$/\1/p' "$src/package.json" | head -n 1)"
out="$src/rift-language-$version.vsix"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/extension"
cp -r "$src/package.json" "$src/language-configuration.json" "$src/extension.js" "$src/insights.js" \
      "$src/syntaxes" "$src/lldb" "$work/extension/"
cp "$src/README.md" "$work/extension/readme.md"
cp "$src/LICENSE" "$work/extension/LICENSE.txt"
chmod -R u+w,go+r "$work"   # VS Code rewrites package.json when it installs: it must be writable
cat > "$work/extension.vsixmanifest" <<XML
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="rift-language" Version="$version" Publisher="dominikm" />
    <DisplayName>Rift Language</DisplayName>
    <Description xml:space="preserve">Rift (.rifts / .rifth): syntax highlighting, build, step-through debugging, and code insights</Description>
    <Tags>debuggers,rift,Rift,__ext_rifts,__ext_rifth</Tags>
    <Categories>Programming Languages,Debuggers</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.75.0" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="vadimcn.vscode-lldb" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value="" />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace" />
      <Property Id="Microsoft.VisualStudio.Code.ExecutesCode" Value="true" />
    </Properties>
    <License>extension/LICENSE.txt</License>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/readme.md" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Content.License" Path="extension/LICENSE.txt" Addressable="true" />
  </Assets>
</PackageManifest>
XML
cat > "$work/[Content_Types].xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension=".json" ContentType="application/json"/><Default Extension=".vsixmanifest" ContentType="text/xml"/><Default Extension=".js" ContentType="application/javascript"/><Default Extension=".md" ContentType="text/markdown"/><Default Extension=".txt" ContentType="text/plain"/><Default Extension=".py" ContentType="application/octet-stream"/></Types>
XML
rm -f "$out"
(cd "$work" && zip -qr "$out" "extension.vsixmanifest" "[Content_Types].xml" extension)
echo "packaged $out"
