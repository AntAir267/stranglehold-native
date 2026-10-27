#!/bin/sh
# UELib's command-line tool (third_party/UELib, built on the local .NET 8 SDK in third_party/dotnet):
#   tools/uelib.sh <decompressed package> "obj list [Class]" | "obj decompile <Group.Object>"
here=$(cd "$(dirname "$0")/.." && pwd)
export DOTNET_ROOT="$here/third_party/dotnet" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
exec "$DOTNET_ROOT/dotnet" "$here/third_party/UELib/CLI/bin/Release/net8.0/Eliot.UELib.CLI.dll" "$@"
