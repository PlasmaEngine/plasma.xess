// #pl-version 1
//
// The Intel XeSS SDK this package builds against and ships. ThirdParty/XeSS is a git submodule of Intel's SDK
// repository, pinned to a release; its signed libxess.dll is copied next to the plugin, which loads it at run time.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageXeSS
{
    public static void ConfigureConsumer(ModuleRules rules, BuildContext context)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("The XeSS package ships Windows binaries only.");
        }

        var sdkRoot = Path.Combine(context.ProjectRoot, "ThirdParty", "XeSS");
        if (!File.Exists(Path.Combine(sdkRoot, "inc", "xess", "xess.h")))
        {
            throw new DirectoryNotFoundException("ThirdParty/XeSS is empty. Run 'git submodule update --init' in the package directory.");
        }
        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "inc"));

        var binaryDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Directory.CreateDirectory(binaryDirectory);
        CopyIfDifferent(Path.Combine(sdkRoot, "bin", "libxess.dll"), Path.Combine(binaryDirectory, "libxess.dll"));
        CopyIfDifferent(Path.Combine(sdkRoot, "LICENSE.txt"), Path.Combine(binaryDirectory, "XeSS-LICENSE.txt"));
        CopyIfDifferent(Path.Combine(sdkRoot, "third-party-programs.txt"), Path.Combine(binaryDirectory, "XeSS-third-party-programs.txt"));
    }

    private static void CopyIfDifferent(string source, string target)
    {
        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"Missing XeSS file '{source}'.", source);
        }

        if (File.Exists(target))
        {
            var from = new FileInfo(source);
            var to = new FileInfo(target);
            if (from.Length == to.Length && from.LastWriteTimeUtc <= to.LastWriteTimeUtc)
            {
                return;
            }
        }

        File.Copy(source, target, true);
    }
}
