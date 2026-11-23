// #pl-version 1
//
// The Intel XeSS SDK this package builds against and ships. ThirdParty/XeSS is a git submodule of Intel's SDK
// repository, pinned to a release; its signed libxess.dll (super resolution), libxess_fg.dll (frame generation) and
// libxell.dll (low latency) are copied next to the plugin, which loads them at run time.

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

        // XeSS-FG and XeLL are linked through their import libraries but delay-loaded: the plugin loads them from its
        // own directory first, so the plugin still loads (and only reports them unsupported) when they are missing.
        rules.PrivateLibraries.Add(Path.Combine(sdkRoot, "lib", "libxess_fg.lib"));
        rules.PrivateLibraries.Add(Path.Combine(sdkRoot, "lib", "libxell.lib"));
        rules.PrivateLibraries.Add("delayimp.lib");
        rules.AdditionalLinkerFlags.Add("/DELAYLOAD:libxess_fg.dll");
        rules.AdditionalLinkerFlags.Add("/DELAYLOAD:libxell.dll");

        var binaryDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Directory.CreateDirectory(binaryDirectory);
        CopyIfDifferent(Path.Combine(sdkRoot, "bin", "libxess.dll"), Path.Combine(binaryDirectory, "libxess.dll"));
        CopyIfDifferent(Path.Combine(sdkRoot, "bin", "libxess_fg.dll"), Path.Combine(binaryDirectory, "libxess_fg.dll"));
        CopyIfDifferent(Path.Combine(sdkRoot, "bin", "libxell.dll"), Path.Combine(binaryDirectory, "libxell.dll"));
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
