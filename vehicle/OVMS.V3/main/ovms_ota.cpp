/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include "esp_log.h"
static const char *TAG = "ota";

#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include "ovms_ota.h"
#include "ovms_command.h"
#include "ovms_config.h"
#include "ovms_metrics.h"
#include "metrics_standard.h"

OvmsOTA MyOTA __attribute__ ((init_priority (4400)));

void ota_status(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
  {
  OvmsMetricString* m = (OvmsMetricString*)MyMetrics.Find(MS_M_VERSION);
  if (m != NULL)
    {
    std::string v = m->AsString();
    writer->printf("Firmware: %s\n",v.c_str());

    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p != NULL)
      {
      writer->printf("Running partition: %s\n",p->label);
      }
    p = esp_ota_get_boot_partition();
    if (p != NULL)
      {
      writer->printf("Boot partition: %s\n",p->label);
      }
    }
  }

void ota_flashvfs(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
  {
  std::string tn = cmd->GetName();

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(running);

  if (running==NULL)
    {
    writer->puts("Error: Current running image cannot be determined - aborting");
    return;
    }
  writer->printf("Current running partition is: %s\n",running->label);

  if (target==NULL)
    {
    writer->puts("Error: Target partition cannot be determined - aborting");
    return;
    }
  writer->printf("Target partition is: %s\n",target->label);

  if (running == target)
    {
    writer->puts("Error: Cannot flash to running image partition");
    return;
    }

  if (MyConfig.ProtectedPath(argv[0]))
    {
    writer->puts("Error: protected path");
    return;
    }

  struct stat ds;
  if (stat(argv[0], &ds) != 0)
    {
    writer->printf("Error: Cannot find file %s\n",argv[0]);
    return;
    }
  writer->printf("Source image is %d bytes in size\n",ds.st_size);

  FILE* f = fopen(argv[0], "r");
  if (f == NULL)
    {
    writer->printf("Error: Cannot open %s\n",argv[0]);
    return;
    }

  writer->puts("Preparing flash partition...");
  esp_ota_handle_t otah;
  esp_err_t err = esp_ota_begin(target, ds.st_size, &otah);
  if (err != ESP_OK)
    {
    writer->printf("Error: ESP32 error #%d when starting OTA operation\n",err);
    return;
    }

  writer->puts("Flashing image partition...");
  char buf[512];
  while(size_t n = fread(buf, sizeof(char), sizeof(buf), f))
    {
    err = esp_ota_write(otah, buf, n);
    if (err != ESP_OK)
      {
      writer->printf("Error: ESP32 error #%d when writing to flash - state is inconsistent\n",err); 
      esp_ota_end(otah);
      fclose(f);
      return;
      }
    }
  fclose(f);

  err = esp_ota_end(otah);
  if (err != ESP_OK)
    {
    writer->printf("Error: ESP32 error #%d finalising OTA operation - state is inconsistent\n",err);
    return;
    }

  fclose(f);

  writer->puts("Setting boot partition...");
  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK)
    {
    writer->printf("Error: ESP32 error #%d setting boot partition - check before rebooting\n",err);
    return;
    }

  writer->printf("OTA flash was successfull\n  Flashed %d bytes from %s\n  Next boot will be from '%s'\n",
                 ds.st_size,argv[0],target->label);
  }

void ota_boot(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
  {
  std::string tn = cmd->GetName();

  esp_partition_subtype_t subtype = ESP_PARTITION_SUBTYPE_ANY;
  if (tn.compare("factory")==0)
    { subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY; }
  else if (tn.compare("ota_0")==0)
    { subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0; }
  else if (tn.compare("ota_1")==0)
    { subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1; }
  if (subtype == ESP_PARTITION_SUBTYPE_ANY) return;

  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
  if (p != NULL)
    {
    esp_err_t err = esp_ota_set_boot_partition(p);
    switch (err)
      {
      case ESP_OK:
        writer->printf("Boot from %s at 0x%08x (size 0x%08x)\n",p->label,p->address,p->size);
        break;
      case ESP_ERR_INVALID_ARG:
        writer->puts("Error: partition argument didn't point to a valid OTA partition of type 'app'");
        break;
      case ESP_ERR_OTA_VALIDATE_FAILED:
        writer->puts("Error: partition contained invalid app image, or signature validation failed");
        break;
      case ESP_ERR_NOT_FOUND:
        writer->puts("Error: OTA partition not found");
        break;
      case ESP_ERR_FLASH_OP_TIMEOUT:
      case ESP_ERR_FLASH_OP_FAIL:
        writer->puts("Error: Flash erase/write failed");
        break;
      default:
        writer->printf("Error: cannot set OTA boot partition (error %d)\n",err);
        break;
      }
    }
  }

void ota_reset(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
  {
  writer->puts("Resetting system...");
  esp_restart();
  }

OvmsOTA::OvmsOTA()
  {
  ESP_LOGI(TAG, "Initialising OTA (4400)");

  MyCommandApp.RegisterCommand("reset","Reset system",ota_reset,"",0,0);

  OvmsCommand* cmd_ota = MyCommandApp.RegisterCommand("ota","OTA framework",NULL,"<$C>",1,1);

  cmd_ota->RegisterCommand("status","Show OTA status",ota_status,"",0,0);

  OvmsCommand* cmd_otaflash = cmd_ota->RegisterCommand("flash","OTA flash",NULL,"<$C>",1,1);
  cmd_otaflash->RegisterCommand("vfs","OTA flash vfs",ota_flashvfs,"<file>",1,1);

  OvmsCommand* cmd_otaboot = cmd_ota->RegisterCommand("boot","OTA boot",NULL,"<$C>",1,1);
  cmd_otaboot->RegisterCommand("factory","Boot from factory image",ota_boot, "", 0, 0);
  cmd_otaboot->RegisterCommand("ota_0","Boot from ota_0 image",ota_boot, "", 0, 0);
  cmd_otaboot->RegisterCommand("ota_1","Boot from ota_1 image",ota_boot, "", 0, 0);
  }

OvmsOTA::~OvmsOTA()
  {
  }