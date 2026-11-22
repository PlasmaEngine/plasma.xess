using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class XeSSPluginModule : ModuleRules
{
    public XeSSPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/XeSSPlugin", "XeSSPluginPCH.h",
            "BUILDSYSTEM_BUILDING_XESSPLUGIN_LIB");

        PlasmaPackageXeSS.ConfigureConsumer(this, context);
    }
}
