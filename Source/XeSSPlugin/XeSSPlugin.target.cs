using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class XeSSPluginTarget : TargetRules
{
    public XeSSPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plXeSSPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("XeSSPluginModule");
    }
}
