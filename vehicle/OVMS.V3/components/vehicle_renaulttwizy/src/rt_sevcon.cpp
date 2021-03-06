/**
 * Project:      Open Vehicle Monitor System
 * Module:       Renault Twizy SEVCON Gen4 access
 *
 * (c) 2017  Michael Balzer <dexter@dexters-web.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "ovms_log.h"
static const char *TAG = "v-twizy";

#include <stdio.h>
#include <string>
#include "ovms_metrics.h"
#include "ovms_events.h"
#include "ovms_config.h"
#include "ovms_command.h"
#include "metrics_standard.h"
#include "ovms_notify.h"

#include "vehicle_renaulttwizy.h"

using namespace std;


/**
 * SevconClient:
 */

SevconClient::SevconClient(OvmsVehicleRenaultTwizy* twizy)
  : m_sync(twizy->m_can1), m_async(twizy->m_can1)
{
  ESP_LOGI(TAG, "sevcon subsystem init");

  m_twizy = twizy;
  m_sevcon_type = 0;

  m_drivemode.u32 = 0;
  m_drivemode.v3tag = 1;
  m_drivemode.profile_user = MyConfig.GetParamValueInt("xrt", "profile_user", 0);
  m_drivemode.profile_cfgmode = MyConfig.GetParamValueInt("xrt", "profile_cfgmode", m_drivemode.profile_user);
  GetParamProfile(m_drivemode.profile_user, m_profile);

  m_cfgmode_request = false;

  // register shell commands:

  OvmsCommand *cmd_cfg = m_twizy->cmd_xrt->RegisterCommand("cfg", "SEVCON tuning", NULL, "", 0, 0, true);

  cmd_cfg->RegisterCommand("pre", "Enter configuration mode (pre-operational)", shell_cfg_mode, "", 0, 0, true);
  cmd_cfg->RegisterCommand("op", "Leave configuration mode (go operational)", shell_cfg_mode, "", 0, 0, true);

  cmd_cfg->RegisterCommand("read", "Read register", shell_cfg_read, "<index_hex> <subindex_hex>", 2, 2, true);
  cmd_cfg->RegisterCommand("write", "Read & write register", shell_cfg_write, "<index_hex> <subindex_hex> <value>", 3, 3, true);
  cmd_cfg->RegisterCommand("writeonly", "Write register", shell_cfg_write, "<index_hex> <subindex_hex> <value>", 3, 3, true);

  cmd_cfg->RegisterCommand("set", "Set tuning profile from base64 string", shell_cfg_set, "<key> <base64data>", 2, 2, true);
  cmd_cfg->RegisterCommand("reset", "Reset tuning profile", shell_cfg_set, "[key]", 0, 1, true);
  cmd_cfg->RegisterCommand("get", "Get tuning profile as base64 string", shell_cfg_get, "[key]", 0, 1, true);
  cmd_cfg->RegisterCommand("info", "Show tuning profile", shell_cfg_info, "[key]", 0, 1, true);
  cmd_cfg->RegisterCommand("save", "Save current tuning profile", shell_cfg_save, "[key]", 0, 1, true);
  cmd_cfg->RegisterCommand("load", "Load stored tuning profile", shell_cfg_load, "[key]", 0, 1, true);

  cmd_cfg->RegisterCommand("drive", "Tune drive power level", shell_cfg_drive,
                            "[max_prc] [autopower_ref] [autopower_minprc] [kickdown_threshold] [kickdown_compzero]", 0, 5, true);
  cmd_cfg->RegisterCommand("recup", "Tune recuperation power levels", shell_cfg_recup,
                            "[neutral_prc] [brake_prc] [autopower_ref] [autopower_minprc]", 0, 4, true);
  cmd_cfg->RegisterCommand("ramps", "Tune pedal reaction", shell_cfg_ramps,
                            "[start_prc] [accel_prc] [decel_prc] [neutral_prc] [brake_prc]", 0, 5, true);
  cmd_cfg->RegisterCommand("ramplimits", "Tune max pedal reaction", shell_cfg_ramplimits,
                            "[accel_prc] [decel_prc]", 0, 2, true);
  cmd_cfg->RegisterCommand("smooth", "Tune pedal smoothing", shell_cfg_smooth,
                            "[prc]", 0, 1, true);
  cmd_cfg->RegisterCommand("speed", "Tune max & warn speed", shell_cfg_speed,
                            "[max_kph] [warn_kph]", 0, 2, true);
  cmd_cfg->RegisterCommand("power", "Tune torque, power & current levels", shell_cfg_power,
                            "[trq_prc] [pwr_lo_prc] [pwr_hi_prc] [curr_prc]", 0, 4, true);
  cmd_cfg->RegisterCommand("tsmap", "Tune torque/speed maps", shell_cfg_tsmap,
                            "[maps] [t1_prc[@t1_spd]] [t2_prc[@t2_spd]] [t3_prc[@t3_spd]] [t4_prc[@t4_spd]]", 0, 5, true);
  cmd_cfg->RegisterCommand("brakelight", "Tune brakelight trigger levels", shell_cfg_brakelight,
                            "[on_lev] [off_lev]", 0, 2, true);

  cmd_cfg->RegisterCommand("showlogs", "Display SEVCON diag logs", shell_cfg_querylogs, "[which=1] [start=0]", 0, 2, true);
  cmd_cfg->RegisterCommand("querylogs", "Send SEVCON diag logs to server", shell_cfg_querylogs, "[which=1] [start=0]", 0, 2, true);
  cmd_cfg->RegisterCommand("clearlogs", "Clear SEVCON diag logs", shell_cfg_clearlogs, "[which=99]", 0, 1, true);

  // TODO:
  //  lock, unlock, valet, unvalet
  // LATER:
  //  getdcf?

  // fault listener:
  m_faultqueue = xQueueCreate(10, sizeof(uint16_t));
  m_lastfault = 0;
  m_buttoncnt = 0;
  using std::placeholders::_1;
  using std::placeholders::_2;
  MyEvents.RegisterEvent(TAG, "canopen.node.emcy", std::bind(&SevconClient::EmcyListener, this, _1, _2));

  m_kickdown_timer = xTimerCreate("RT kickdown", pdMS_TO_TICKS(100), pdTRUE, NULL, KickdownTimer);
}

SevconClient::~SevconClient()
{
  if (m_kickdown_timer)
    xTimerDelete(m_kickdown_timer, 0);
  if (m_faultqueue)
    vQueueDelete(m_faultqueue);
}

SevconClient* SevconClient::GetInstance(OvmsWriter* writer /*=NULL*/)
{
  OvmsVehicleRenaultTwizy* twizy = OvmsVehicleRenaultTwizy::GetInstance(writer);
  if (twizy)
    return twizy->GetSevconClient();
  else
    return NULL;
}


SevconJob::SevconJob(SevconClient* client)
{
  m_client = client;
}

SevconJob::SevconJob(OvmsVehicleRenaultTwizy* twizy)
{
  m_client = twizy->GetSevconClient();
}

SevconJob::~SevconJob()
{
}


uint32_t SevconClient::GetDeviceError()
{
  // get SEVCON specific error code from 0x5310.00:
  CANopenJob errjob;
  uint32_t device_error = 0;
  if (m_sync.ReadSDO(errjob, m_nodeid, 0x5310, 0x00, (uint8_t*)&device_error, 4) == COR_OK)
    return 0xde000000 | device_error;
  return CANopen_GeneralError;
}


CANopenResult_t SevconClient::CheckBus()
{
  // check for CAN write access:
  if (!m_twizy->twizy_flags.EnableWrite)
    return COR_ERR_NoCANWrite;

  // check component status (currently only SEVCON):
  if ((m_twizy->twizy_status & CAN_STATUS_KEYON) == 0)
    return COR_ERR_DeviceOffline;

  return COR_OK;
}


CANopenResult_t SevconClient::Read(CANopenJob& job, uint16_t index, uint8_t subindex, uint8_t* buf, size_t bufsize)
{
  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return job.SetResult(res);
  res = m_sync.ReadSDO(job, m_nodeid, index, subindex, buf, bufsize);
  if (res != COR_OK && job.sdo.error == CANopen_GeneralError && index != 0x5310)
    job.sdo.error = GetDeviceError();
  if (res != COR_OK)
    ESP_LOGD(TAG, "Sevcon ReadSDO 0x%04x.%02x failed: %s", index, subindex, GetResultString(job).c_str());
  return res;
}

CANopenResult_t SevconClient::Write(CANopenJob& job, uint16_t index, uint8_t subindex, uint8_t* buf, size_t bufsize)
{
  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return job.SetResult(res);
  res = m_sync.WriteSDO(job, m_nodeid, index, subindex, buf, bufsize);
  if (res != COR_OK && job.sdo.error == CANopen_GeneralError && index != 0x5310)
    job.sdo.error = GetDeviceError();
  if (res != COR_OK)
    ESP_LOGD(TAG, "Sevcon WriteSDO 0x%04x.%02x failed: %s", index, subindex, GetResultString(job).c_str());
  return res;
}

CANopenResult_t SevconClient::RequestState(CANopenJob& job, CANopenNMTCommand_t command, bool wait_for_state)
{
  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return job.SetResult(res);
  res = m_sync.SendNMT(job, m_nodeid, command, wait_for_state);
  if (res != COR_OK)
    ESP_LOGD(TAG, "Sevcon state request %s failed: %s",
      CANopen::GetCommandName(command).c_str(), GetResultString(job).c_str());
  return res;
}

CANopenResult_t SevconClient::GetHeartbeat(CANopenJob& job, CANopenNMTState_t& var)
{
  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return job.SetResult(res);
  res = m_sync.ReceiveHB(job, m_nodeid, &var);
  if (res != COR_OK)
    ESP_LOGD(TAG, "Sevcon get heartbeat failed: %s", GetResultString(job).c_str());
  return res;
}

CANopenResult_t SevconClient::SendWrite(uint16_t index, uint8_t subindex, uint32_t* value)
{
  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return res;
  return m_async.WriteSDO(m_nodeid, index, subindex, (uint8_t*)value, 0);
}

CANopenResult_t SevconClient::SendRequestState(CANopenNMTCommand_t command)
{
  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return res;
  return m_async.SendNMT(m_nodeid, command);
}


void SevconClient::ProcessAsyncResults()
{
  CANopenJob job;
  while (m_async.ReceiveDone(job, 0) != COR_ERR_QueueEmpty) {
    ESP_LOGD(TAG, "Sevcon async result for %s: %s",
      CANopen::GetJobName(job).c_str(), GetResultString(job).c_str());
  }
}


CANopenResult_t SevconClient::Login(bool on)
{
  ESP_LOGD(TAG, "Sevcon login request: %d", on);
  SevconJob sc(this);

  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return res;

  // get SEVCON type (Twizy 80/45):
  if (sc.Read(0x1018, 0x02, m_sevcon_type) != COR_OK)
    return COR_ERR_UnknownDevice;

  if (MyConfig.GetParamValue("xrt", "type") == "SC80GB45") {
    ESP_LOGD(TAG, "Twizy type 2 = SC80GB45");
    m_drivemode.type = 2; // SC80GB45
  }
  else if (m_sevcon_type == SC_Gen4_4845) {
    ESP_LOGD(TAG, "Twizy type 0 = Twizy80");
    m_drivemode.type = 0; // Twizy80
  }
  else if (m_sevcon_type == SC_Gen4_4827) {
    ESP_LOGD(TAG, "Twizy type 1 = Twizy45");
    m_drivemode.type = 1; // Twizy45
  }
  else {
    ESP_LOGE(TAG, "Twizy type unknown: SEVCON type 0x%08x", m_sevcon_type);
    return COR_ERR_UnknownDevice;
  }

  // check login level:
  uint32_t level;
  if (sc.Read(0x5000, 0x01, level) != COR_OK)
    return COR_ERR_LoginFailed;

  if (on && level != 4) {
    // login:
    sc.Write(0x5000, 0x03, 0);
    sc.Write(0x5000, 0x02, 0x4bdf);

    // check new level:
    sc.Read(0x5000, 0x01, level);
    if (level != 4)
      return COR_ERR_LoginFailed;
  }

  else if (!on && level != 0) {
    // logout:
    sc.Write(0x5000, 0x03, 0);
    sc.Write(0x5000, 0x02, 0);

    // check new level:
    sc.Read(0x5000, 0x01, level);
    if (level != 0)
      return COR_ERR_LoginFailed;
  }

  ESP_LOGD(TAG, "Sevcon login status: %d", on);
  SetCtrlLoggedIn(on);
  if (on)
    MyEvents.SignalEvent("vehicle.ctrl.loggedin", NULL);
  return COR_OK;
}


CANopenResult_t SevconClient::CfgMode(CANopenJob& job, bool on)
{
  // Note: as waiting for the next heartbeat would take ~ 500 ms, so
  //  we read the state change result from 0x5110.00

  ESP_LOGD(TAG, "Sevcon cfgmode request: %d", on);
  uint32_t state = 0;
  m_cfgmode_request = on;

  CANopenResult_t res = CheckBus();
  if (res != COR_OK)
    return res;

  if (!on)
  {
    // request operational state:
    if (RequestState(job, CONC_Start, false) != COR_OK)
      return COR_ERR_StateChangeFailed;

    // give controller some time…
    vTaskDelay(pdMS_TO_TICKS(10));

    // check state:
    Read(job, 0x5110, 0x00, state);
    if (state != CONS_Operational)
      return COR_ERR_StateChangeFailed;
  }
  else
  {
    // Triple (redundancy) safety check:
    //   only allow preop mode attempt if...
    //    a) Twizy is not moving
    //    b) and in "N" mode
    //    c) and not in "GO" mode
    if ((m_twizy->twizy_speed != 0) || (m_twizy->twizy_status & (CAN_STATUS_MODE | CAN_STATUS_GO)))
      return COR_ERR_StateChangeFailed;

    // request pre-operational state:
    if (RequestState(job, CONC_PreOp, false) != COR_OK)
      return COR_ERR_StateChangeFailed;

    // give controller some time…
    vTaskDelay(pdMS_TO_TICKS(10));

    // check state:
    Read(job, 0x5110, 0x00, state);
    if (state != CONS_PreOperational)
    {
      // clear preop request:
      CANopenJob job2;
      RequestState(job2, CONC_Start, false);
      return COR_ERR_StateChangeFailed;
    }
  }

  ESP_LOGD(TAG, "Sevcon cfgmode status: %d", on);
  SetCtrlCfgMode(on);
  if (on)
    MyEvents.SignalEvent("vehicle.ctrl.cfgmode", NULL);
  else
    MyEvents.SignalEvent("vehicle.ctrl.runmode", NULL);
  return COR_OK;
}


void SevconClient::SetStatus(bool car_awake)
{
  SetCtrlLoggedIn(false);
  SetCtrlCfgMode(false);
  m_buttoncnt = 0;
}


void SevconClient::Ticker1(uint32_t ticker)
{
  // update metrics:
  StdMetrics.ms_v_env_drivemode->SetValue((long) m_drivemode.u32);

  // cleanup CANopen job result queue:
  ProcessAsyncResults();

  // Send fault alerts:
  uint16_t faultcode;
  while (xQueueReceive(m_faultqueue, &faultcode, 0) == pdTRUE) {
    SendFaultAlert(faultcode);
  }

  if (!m_twizy->twizy_flags.CarAwake || !m_twizy->twizy_flags.EnableWrite)
    return;

  // Login to SEVCON:
  if (!CtrlLoggedIn()) {
    if (Login(true) != COR_OK)
      return;
  }

  // Check for 3 successive button presses in STOP mode => CFG RESET:
  if ((m_buttoncnt >= 5) && (!m_twizy->twizy_flags.DisableReset)) {
    ESP_LOGW(TAG, "Sevcon: detected D/R button cfg reset request");

    // reset current profile:
    GetParamProfile(0, m_profile);
    CANopenResult_t res = CfgApplyProfile(m_drivemode.profile_user);
    m_drivemode.unsaved = (m_drivemode.profile_user > 0);

    // send result:
    MyNotify.NotifyStringf("info", "xrt.sevcon.reset", "Tuning RESET: %s\n", FmtSwitchProfileResult(res).c_str());

    // reset button cnt:
    m_buttoncnt = 0;
  }
  else if (m_buttoncnt > 0) {
    m_buttoncnt--;
  }

  // Auto drive & recuperation adjustment (if enabled):
  CfgAutoPower();

#if 0 // TODO
  // Valet mode: lock speed if valet max odometer reached:
  if (ValetMode() && !CarLocked() && twizy_odometer > twizy_valet_odo)
  {
    vehicle_twizy_cfg_restrict_cmd(FALSE, CMD_Lock, NULL);
  }
#endif
}
