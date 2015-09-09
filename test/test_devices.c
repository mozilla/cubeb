/*
 * Copyright © 2015 Haakon Sporsheim <haakon.sporsheim@telenordigital.com>
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

/* libcubeb enumerate device test/example.
 * Prints out a list of devices enumerated. */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cubeb/cubeb.h"


static void
print_device_list(cubeb * context, cubeb_device_list * devices, FILE * f)
{
  char * devid = NULL, devfmts[64] = "";
  const char * devtype, * devstate, * devdeffmt;

  if (devices == NULL)
    return;

  cubeb_device_id_to_str(context, devices->device.device_id, &devid);

  switch (devices->device.type) {
    case CUBEB_DEVICE_TYPE_INPUT:
      devtype = "input";
      break;
    case CUBEB_DEVICE_TYPE_OUTPUT:
      devtype = "output";
      break;
    case CUBEB_DEVICE_TYPE_UNKNOWN:
    default:
      devtype = "unknown?";
      break;
  };

  switch (devices->device.state) {
    case CUBEB_DEVICE_STATE_DISABLED:
      devstate = "disabled";
      break;
    case CUBEB_DEVICE_STATE_UNPLUGGED:
      devstate = "unplugged";
      break;
    case CUBEB_DEVICE_STATE_ENABLED:
      devstate = "enabled";
      break;
    default:
      devstate = "unknown?";
      break;
  };

  switch (devices->device.default_format) {
    case CUBEB_DEVICE_FMT_S16LE:
      devdeffmt = "S16LE";
      break;
    case CUBEB_DEVICE_FMT_S16BE:
      devdeffmt = "S16BE";
      break;
    case CUBEB_DEVICE_FMT_F32LE:
      devdeffmt = "F32LE";
      break;
    case CUBEB_DEVICE_FMT_F32BE:
      devdeffmt = "F32BE";
      break;
    default:
      devdeffmt = "unknown?";
      break;
  };

  if (devices->device.format & CUBEB_DEVICE_FMT_S16LE)
    strcat(devfmts, " S16LE");
  if (devices->device.format & CUBEB_DEVICE_FMT_S16BE)
    strcat(devfmts, " S16BE");
  if (devices->device.format & CUBEB_DEVICE_FMT_F32LE)
    strcat(devfmts, " F32LE");
  if (devices->device.format & CUBEB_DEVICE_FMT_F32BE)
    strcat(devfmts, " F32BE");

  fprintf(f,
      "dev: \"%s\"%s\n"
      "\tName:    \"%s\"\n"
      "\tGroup:   \"%s\"\n"
      "\tVendor:  \"%s\"\n"
      "\tType:    %s\n"
      "\tState:   %s\n"
      "\tCh:      %u\n"
      "\tFormat:  %s (0x%x) (default: %s)\n"
      "\tRate:    %u - %u (default: %u)\n"
      "\tLatency: lo %ums, hi %ums\n",
      devid, devices->device.preferred ? " (PREFERRED)" : "",
      devices->device.friendly_name, devices->device.group_id,
      devices->device.vendor_name, devtype, devstate,
      devices->device.max_channels,
      (devfmts[0] == ' ') ? &devfmts[1] : devfmts,
      (unsigned int)devices->device.format, devdeffmt,
      devices->device.min_rate, devices->device.max_rate,
      devices->device.default_rate,
      devices->device.latency_lo_ms, devices->device.latency_hi_ms);

  free(devid);

  print_device_list(context, devices->next, f);
}

static int
run_enumerate_devices(void)
{
  int r = CUBEB_OK;
  cubeb * ctx = NULL;
  cubeb_device_list * devices = NULL;
  uint32_t count = 0;

  r = cubeb_init(&ctx, "Cubeb audio test");
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error initializing cubeb library\n");
    return r;
  }

  fprintf(stdout, "Enumerating input devices for backend %s\n",
      cubeb_get_backend_id (ctx));

  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_INPUT, &devices, &count);
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error enumerating devices %d\n", r);
    goto cleanup;
  }

  fprintf(stdout, "Found %u input devices\n", count);
  print_device_list (ctx, devices, stdout);
  cubeb_device_list_destroy (ctx, devices);

  fprintf(stdout, "Enumerating output devices for backend %s\n",
          cubeb_get_backend_id (ctx));

  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &devices, &count);
  if (r != CUBEB_OK) {
    fprintf(stderr, "Error enumerating devices %d\n", r);
    goto cleanup;
  }

  fprintf(stdout, "Found %u output devices\n", count);
  print_device_list (ctx, devices, stdout);
  cubeb_device_list_destroy (ctx, devices);

cleanup:
  cubeb_destroy(ctx);
  return r;
}

int main(int argc, char *argv[])
{
  int ret;

  ret = run_enumerate_devices();

  return ret;
}
