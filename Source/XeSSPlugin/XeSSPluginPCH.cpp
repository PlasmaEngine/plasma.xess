#include <XeSSPlugin/XeSSPluginPCH.h>

#include <Foundation/Configuration/Plugin.h>

PL_STATICLINK_LIBRARY(XeSSPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(XeSSPlugin_XeSSUpscaler);
}
