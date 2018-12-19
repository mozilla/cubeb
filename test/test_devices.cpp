/*
 * Copyright © 2015 Haakon Sporsheim <haakon.sporsheim@telenordigital.com>
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

/* libcubeb enumerate device test/example.
 * Prints out a list of devices enumerated. */
#include "gtest/gtest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include "cubeb/cubeb.h"

//#define ENABLE_NORMAL_LOG
//#define ENABLE_VERBOSE_LOG
#include "common.h"

long data_cb_duplex(cubeb_stream * stream, void * user, const void * inputbuffer, void * outputbuffer, long nframes)
{
  // noop, unused
  return 0;
}

void state_cb_duplex(cubeb_stream * stream, void * /*user*/, cubeb_state state)
{
  // noop, unused
}

static void
print_device_info(cubeb_device_info * info, FILE * f)
{
  char devfmts[64] = "";
  const char * devtype, * devstate, * devdeffmt;

  switch (info->type) {
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

  switch (info->state) {
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

  switch (info->default_format) {
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

  if (info->format & CUBEB_DEVICE_FMT_S16LE)
    strcat(devfmts, " S16LE");
  if (info->format & CUBEB_DEVICE_FMT_S16BE)
    strcat(devfmts, " S16BE");
  if (info->format & CUBEB_DEVICE_FMT_F32LE)
    strcat(devfmts, " F32LE");
  if (info->format & CUBEB_DEVICE_FMT_F32BE)
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
      "\tLatency: lo %u frames, hi %u frames\n",
      info->device_id, info->preferred ? " (PREFERRED)" : "",
      info->friendly_name, info->group_id, info->vendor_name,
      devtype, devstate, info->max_channels,
      (devfmts[0] == '\0') ? devfmts : devfmts + 1,
      (unsigned int)info->format, devdeffmt,
      info->min_rate, info->max_rate, info->default_rate,
      info->latency_lo, info->latency_hi);
}

static void
print_device_collection(cubeb_device_collection * collection, FILE * f)
{
  uint32_t i;

  for (i = 0; i < collection->count; i++)
    print_device_info(&collection->device[i], f);
}

TEST(cubeb, destroy_default_collection)
{
  int r;
  cubeb * ctx = NULL;
  cubeb_device_collection collection{ nullptr, 0 };

  r = common_init(&ctx, "Cubeb audio test");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  ASSERT_EQ(collection.device, nullptr);
  ASSERT_EQ(collection.count, (size_t) 0);

  r = cubeb_device_collection_destroy(ctx, &collection);
  if (r != CUBEB_ERROR_NOT_SUPPORTED) {
    ASSERT_EQ(r, CUBEB_OK);
    ASSERT_EQ(collection.device, nullptr);
    ASSERT_EQ(collection.count, (size_t) 0);
  }
}

TEST(cubeb, enumerate_devices)
{
  int r;
  cubeb * ctx = NULL;
  cubeb_device_collection collection;

  r = common_init(&ctx, "Cubeb audio test");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  fprintf(stdout, "Enumerating input devices for backend %s\n",
      cubeb_get_backend_id(ctx));

  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_INPUT, &collection);
  if (r == CUBEB_ERROR_NOT_SUPPORTED) {
    fprintf(stderr, "Device enumeration not supported"
                    " for this backend, skipping this test.\n");
    r = CUBEB_OK;
  }
  ASSERT_EQ(r, CUBEB_OK) << "Error enumerating devices " << r;

  fprintf(stdout, "Found %zu input devices\n", collection.count);
  print_device_collection(&collection, stdout);
  cubeb_device_collection_destroy(ctx, &collection);

  fprintf(stdout, "Enumerating output devices for backend %s\n",
          cubeb_get_backend_id(ctx));

  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &collection);
  ASSERT_EQ(r, CUBEB_OK) << "Error enumerating devices " << r;

  fprintf(stdout, "Found %zu output devices\n", collection.count);
  print_device_collection(&collection, stdout);
  cubeb_device_collection_destroy(ctx, &collection);

  uint32_t count_before_creating_duplex_stream;
  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &collection);
  ASSERT_EQ(r, CUBEB_OK) << "Error enumerating devices " << r;
  count_before_creating_duplex_stream = collection.count;
  cubeb_device_collection_destroy(ctx, &collection);

  cubeb_stream * stream;
  cubeb_stream_params input_params;
  cubeb_stream_params output_params;

  input_params.format = output_params.format = CUBEB_SAMPLE_FLOAT32NE;
  input_params.rate = output_params.rate = 48000;
  input_params.channels = output_params.channels = 1;
  input_params.layout = output_params.layout = CUBEB_LAYOUT_MONO;
  input_params.prefs = output_params.prefs = CUBEB_STREAM_PREF_NONE;

  r = cubeb_stream_init(ctx, &stream, "Cubeb duplex",
                        NULL, &input_params, NULL, &output_params,
                        1024, data_cb_duplex, state_cb_duplex, nullptr);

  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb stream";

  r = cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT, &collection);
  ASSERT_EQ(r, CUBEB_OK) << "Error enumerating devices " << r;
  ASSERT_EQ(count_before_creating_duplex_stream, collection.count);
  cubeb_device_collection_destroy(ctx, &collection);

  cubeb_stream_destroy(stream);
}

TEST(cubeb, stream_get_current_device)
{
  cubeb * ctx = NULL;
  int r = common_init(&ctx, "Cubeb audio test");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  fprintf(stdout, "Getting current devices for backend %s\n",
    cubeb_get_backend_id(ctx));

  cubeb_stream * stream = NULL;
  cubeb_stream_params input_params;
  cubeb_stream_params output_params;

  input_params.format = output_params.format = CUBEB_SAMPLE_FLOAT32NE;
  input_params.rate = output_params.rate = 48000;
  input_params.channels = output_params.channels = 1;
  input_params.layout = output_params.layout = CUBEB_LAYOUT_MONO;
  input_params.prefs = output_params.prefs = CUBEB_STREAM_PREF_NONE;

  r = cubeb_stream_init(ctx, &stream, "Cubeb duplex",
                        NULL, &input_params, NULL, &output_params,
                        1024, data_cb_duplex, state_cb_duplex, nullptr);
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb stream";
  std::unique_ptr<cubeb_stream, decltype(&cubeb_stream_destroy)>
    cleanup_stream_at_exit(stream, cubeb_stream_destroy);

  cubeb_device * device;
  r = cubeb_stream_get_current_device(stream, &device);
  if (r == CUBEB_ERROR_NOT_SUPPORTED) {
    fprintf(stderr, "Getting current device is not supported"
                    " for this backend, skipping this test.\n");
    return;
  }
  ASSERT_EQ(r, CUBEB_OK) << "Error getting current devices";

  fprintf(stdout, "Current output device: %s\n", device->output_name);
  fprintf(stdout, "Current input device: %s\n", device->input_name);

  r = cubeb_stream_device_destroy(stream, device);
  ASSERT_EQ(r, CUBEB_OK) << "Error destroying current devices";
}

void device_collection_changed_callback(cubeb * context, void * user)
{
  fprintf(stderr, "device collection changed callback\n");
  ASSERT_TRUE(false) << "Error: device collection changed callback"
                        " called without changing devices";
}

TEST(cubeb, register_device_collection_change_for_unknown_type)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  r = cubeb_register_device_collection_changed(ctx,
                                               CUBEB_DEVICE_TYPE_UNKNOWN,
                                               nullptr,
                                               nullptr);
  ASSERT_EQ(r, CUBEB_ERROR_INVALID_PARAMETER) << "Error returning wrong error type";

  r = cubeb_register_device_collection_changed(ctx,
                                               CUBEB_DEVICE_TYPE_UNKNOWN,
                                               device_collection_changed_callback,
                                               nullptr);
  ASSERT_EQ(r, CUBEB_ERROR_INVALID_PARAMETER) << "Error returning wrong error type";
}

TEST(cubeb, unregister_without_registering)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  cubeb_device_type scopes[3] = {
    CUBEB_DEVICE_TYPE_INPUT,
    CUBEB_DEVICE_TYPE_OUTPUT,
    static_cast<cubeb_device_type>(CUBEB_DEVICE_TYPE_INPUT |
                                   CUBEB_DEVICE_TYPE_OUTPUT)
  };

  for (cubeb_device_type scope: scopes) {
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 nullptr,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error unregistering device collection changed";
  }
}

TEST(cubeb, device_collection_change)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  cubeb_device_type scopes[3] = {
    CUBEB_DEVICE_TYPE_INPUT,
    CUBEB_DEVICE_TYPE_OUTPUT,
    static_cast<cubeb_device_type>(CUBEB_DEVICE_TYPE_INPUT |
                                   CUBEB_DEVICE_TYPE_OUTPUT)
  };

  for (cubeb_device_type scope: scopes) {
    // Register a callback within the defined scoped.
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 device_collection_changed_callback,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error registering device collection changed";

    // Unregister all the callbacks regardless of the scopes.
    r = cubeb_register_device_collection_changed(ctx,
                                                 static_cast<cubeb_device_type>(CUBEB_DEVICE_TYPE_INPUT |
                                                                                CUBEB_DEVICE_TYPE_OUTPUT),
                                                 nullptr,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error unregistering all device collection changed";

    // Register a callback within the defined scoped again.
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 device_collection_changed_callback,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error registering device collection changed again";

    // Unregister the callback within the defined scope.
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 nullptr,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error unregistering device collection changed";
  }
}

TEST(cubeb, unregister_device_collection_changed_twice)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  cubeb_device_type scopes[3] = {
    CUBEB_DEVICE_TYPE_INPUT,
    CUBEB_DEVICE_TYPE_OUTPUT,
    static_cast<cubeb_device_type>(CUBEB_DEVICE_TYPE_INPUT |
                                   CUBEB_DEVICE_TYPE_OUTPUT)
  };

  for (cubeb_device_type scope: scopes) {
    // Register a callback within the defined scoped.
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 device_collection_changed_callback,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error registering device collection changed";

    // Unregister the callback within the defined scope.
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 nullptr,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error unregistering device collection changed";

    // Unregister the callback within the defined scope again.
    r = cubeb_register_device_collection_changed(ctx,
                                                 scope,
                                                 nullptr,
                                                 nullptr);
    ASSERT_EQ(r, CUBEB_OK) << "Error unregistering device collection changed again";
  }
}

TEST(cubeb, register_device_collection_changed_twice_input)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  // Register a callback within the defined scoped.
  r = cubeb_register_device_collection_changed(ctx,
                                               CUBEB_DEVICE_TYPE_INPUT,
                                               device_collection_changed_callback,
                                               nullptr);
  ASSERT_EQ(r, CUBEB_OK) << "Error registering device collection changed";

  // Get an assertion fails when registering a callback within same scope twice.
  ASSERT_DEATH(
    cubeb_register_device_collection_changed(ctx,
                                             CUBEB_DEVICE_TYPE_INPUT,
                                             device_collection_changed_callback,
                                             nullptr),
    ""
  );
}

TEST(cubeb, register_device_collection_changed_twice_output)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  // Register a callback within the defined scoped.
  r = cubeb_register_device_collection_changed(ctx,
                                               CUBEB_DEVICE_TYPE_OUTPUT,
                                               device_collection_changed_callback,
                                               nullptr);
  ASSERT_EQ(r, CUBEB_OK) << "Error registering device collection changed";

  // Get an assertion fails when registering a callback within same scope twice.
  ASSERT_DEATH(
    cubeb_register_device_collection_changed(ctx,
                                             CUBEB_DEVICE_TYPE_OUTPUT,
                                             device_collection_changed_callback,
                                             nullptr),
    ""
  );
}

TEST(cubeb, register_device_collection_changed_twice_inout)
{
  cubeb *ctx;
  int r = CUBEB_OK;

  r = common_init(&ctx, "Cubeb duplex example with collection change");
  ASSERT_EQ(r, CUBEB_OK) << "Error initializing cubeb library";

  std::unique_ptr<cubeb, decltype(&cubeb_destroy)>
    cleanup_cubeb_at_exit(ctx, cubeb_destroy);

  cubeb_device_type type = static_cast<cubeb_device_type>(
    CUBEB_DEVICE_TYPE_INPUT | CUBEB_DEVICE_TYPE_OUTPUT);

  // Register a callback within the defined scoped.
  r = cubeb_register_device_collection_changed(ctx,
                                               type,
                                               device_collection_changed_callback,
                                               nullptr);
  ASSERT_EQ(r, CUBEB_OK) << "Error registering device collection changed";

  // Get an assertion fails when registering a callback within same scope twice.
  ASSERT_DEATH(
    cubeb_register_device_collection_changed(ctx,
                                             type,
                                             device_collection_changed_callback,
                                             nullptr),
    ""
  );
}