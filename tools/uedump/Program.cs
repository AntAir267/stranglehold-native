// uedump: dump objects and decompile UnrealScript from decompressed Stranglehold packages (UELib).
//
//   uedump <script package folder> <package.upk> <output folder> <path prefix> [<path prefix> ...]
//   uedump <script package folder> <package.upk> <output folder> --classes
//
// Every .upk in the script package folder is read first and each array property's element type
// is recorded (UELib only finds a property's type when its class is in the same package; the
// game's classes live in the script packages). Then every object of the package whose path
// (Outer.Name) starts with one of the prefixes is decompiled into <output>/<path>.txt: a class
// gives its UnrealScript, any other object its properties.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UELib;
using UELib.Core;
using UELib.Types;

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length < 4)
        {
            Console.Error.WriteLine("usage: uedump <script package folder> <package.upk> <output folder> <path prefix>...");
            return 2;
        }
        string scripts = args[0], target = args[1], outDir = args[2];
        var prefixes = args.Skip(3).ToArray();
        bool classesOnly = prefixes.Length == 1 && prefixes[0] == "--classes";   // every class's script
        Directory.CreateDirectory(outDir);

        var types = new Dictionary<string, Tuple<string, PropertyType>>();
        foreach (var file in Directory.GetFiles(scripts, "*.upk").OrderBy(f => f))
        {
            try
            {
                var p = UnrealLoader.LoadPackage(file);
                p.InitializePackage(UnrealPackage.InitFlags.Construct | UnrealPackage.InitFlags.RegisterClasses);
                foreach (var a in p.Objects.OfType<UArrayProperty>())
                {
                    try
                    {
                        a.BeginDeserializing();
                        if (a.InnerProperty != null && !types.ContainsKey(a.Name))
                            types[a.Name] = Tuple.Create(a.Outer?.Name.ToString(), a.InnerProperty.Type);
                    }
                    catch (Exception) { }
                }
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"{Path.GetFileName(file)}: {e.Message}");
            }
        }
        UnrealConfig.VariableTypes = types;
        Console.WriteLine($"{types.Count} array property types from {scripts}");

        var pkg = UnrealLoader.LoadPackage(target);
        pkg.InitializePackage(UnrealPackage.InitFlags.Construct | UnrealPackage.InitFlags.RegisterClasses);
        int n = 0;
        foreach (var obj in pkg.Objects)
        {
            if (obj.ExportTable == null) continue;            // imports
            string path = obj.GetPath();
            if (classesOnly ? !(obj is UClass)
                            : !prefixes.Any(x => path.StartsWith(x, StringComparison.OrdinalIgnoreCase))) continue;
            string text;
            try
            {
                obj.BeginDeserializing();
                text = obj.Decompile();
            }
            catch (Exception e)
            {
                text = "/* decompile failed: " + e.Message + " */";
            }
            string safe = string.Concat(path.Select(ch => Path.GetInvalidFileNameChars().Contains(ch) ? '_' : ch));
            File.WriteAllText(Path.Combine(outDir, safe + ".txt"), $"// {obj.GetReferencePath()}\n" + text.Replace("\r\n", "\n"));
            n++;
        }
        Console.WriteLine($"{n} objects written to {outDir}");
        return 0;
    }
}
