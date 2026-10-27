#!/bin/sh
# tools/uedump (our dump tool on UELib), run on the local .NET SDK in third_party/dotnet:
#   tools/uedump.sh <script package folder> <package.upk> <output folder> <path prefix>...
here=$(cd "$(dirname "$0")/.." && pwd)
export DOTNET_ROOT="$here/third_party/dotnet" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
exec "$DOTNET_ROOT/dotnet" "$here/work/uedump/bin/Release/net8.0/uedump.dll" "$@"
