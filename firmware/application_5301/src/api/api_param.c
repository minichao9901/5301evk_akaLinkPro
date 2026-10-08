/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

#include "api_param.h"
#include "usb_composite.h"
#include "cdc_interface.h"
#include "rtt_bridge.h"
#include "riscv_svc.h"
#include "scope_sampler.h"
#include "spi_bridge.h"
#include "spi_cdc.h"
#include "i2c_bridge.h"
#include "bus_periodic.h"
#include "analog_bridge.h"
#include "SW_DP.h"
#include "led_state.h"
#include "hpm_dfu_trigger.h"
#include "board.h"

#include <string.h>
#include "hpm_common.h"
#include "hpm_ppor_drv.h"
#include "easyflash.h"

/* Authoritative service flags: cold gate reads words, producers write bytes. */
service_gate_t api_param_gate;
#define s_save_pending (api_param_gate.flag[0])

/* Firmware metadata injected at build time by firmware/tools/pack.py.
 * See Firmware_Integrity_Plan.md for the layout. */
#define APP_HEADER_BASE (0x80020000UL)
#define APPLICATION_CODE_LENGTH_ADDR (APP_HEADER_BASE + 0x04)
#define APPLICATION_CODE_CRC32_ADDR (APP_HEADER_BASE + 0x08)
#define APPLICATION_VER_STR_ADDR ((const char *)(APP_HEADER_BASE + 0x10))
#define APPLICATION_TS_STR_ADDR ((const char *)(APP_HEADER_BASE + 0x18))
#define APPLICATION_DESC_STR_ADDR ((const char *)(APP_HEADER_BASE + 0x2C))

#define BOOTLOADER_START_ADDR (0x80000000UL)
#define BL_INFO_BASE (0x8001F000UL)
#define BOOTLOADER_VER_STR_ADDR ((const char *)(BL_INFO_BASE + 0x10))
#define BOOTLOADER_TS_STR_ADDR ((const char *)(BL_INFO_BASE + 0x18))
#define HARDWARE_VER_STR_ADDR ((const char *)(BL_INFO_BASE + 0x2C))
#define HARDWARE_PROD_TS_STR_ADDR ((const char *)(BL_INFO_BASE + 0x34))

#define CMD_NOT_SUPPORT (0x00)
#define CMD_GET_CONFIG (0x01)
#define CMD_SET_CONFIG (0x02)
#define CMD_GET_VOLTAGE (0x03)
#define CMD_SAVE_CONFIG (0x04)
#define CMD_GET_MODEL (0x10)
#define CMD_GET_SERIAL (0x11)
#define CMD_GET_HWVER (0x12)
#define CMD_GET_FWVER (0x13)
#define CMD_GET_BLVER (0x14)
#define CMD_GET_HW_PROD_DATE (0x15)
#define CMD_GET_FW_COMPILE_DATE (0x16)
#define CMD_GET_BL_COMPILE_DATE (0x17)
#define CMD_UART_DIAG (0x18)
#define CMD_RESET_DEVICE (0xFE)
#define CMD_ENTER_DFU (0xFF)
/* Probe-side SEGGER RTT bridge (see src/rtt/rtt_bridge.c). */
#define CMD_RTT (0x31)
#define RTT_ACT_STOP 0U
#define RTT_ACT_START 1U
#define RTT_ACT_STATUS 2U
#define RTT_ACT_AUTOSTART 3U
#define RTT_ACT_RAW_DAP 4U
#define RTT_ACT_PEEK 5U
#define RTT_ACT_RAW_RESULT 6U
#define RTT_ACT_CONFIG 7U
#define RTT_ACT_BENCH 8U
#define RTT_ACT_BENCH_RESULT 9U
#define RTT_ACT_TARGET 10U

/* Probe-side RISC-V (JTAG) memory engine (see src/riscv/).
 * 原本在 0x32，因网页侧「J-Scope 波形」页把 SCOPE 定在 0x32（另一仓库
 * web-serial-rtt-tools 的 app/scope/protocol.js 里写死 HID_CMD = 0x32），
 * 这里让位挪到 0x33。改这里要同步 script_test/hpm6800_*.py 与 Custom HID Protocol.md。 */
#define CMD_RISCV (0x33)

/* ---- CMD 0x32 SCOPE：探针侧 HSS 采样（J-Scope 波形页的数据源）----
 * 形状照抄 0x31：res_hid[3] = 动作号回显，res_hid[4..] = 12 个状态字。
 * ⚠️ 与 0x31 的差别：网页对 0x32 是把**启动码放在 payload[2]** 读的
 * （app/scope/view.js 就是这么等 -100 变 0 的），所以 payload[2] = startRc。 */
#define CMD_SCOPE (0x32)
#define SCOPE_ACT_STOP 0U
#define SCOPE_ACT_START 1U
#define SCOPE_ACT_STATUS 2U
#define SCOPE_ACT_CLOCK 3U
#define SCOPE_ACT_TRIGGER 4U      /* v2：探针侧触发，当前主机侧触发已够用 */
#define SCOPE_ACT_CONFIG 7U
#define SCOPE_ACT_CONFIG_TICKS 10U
#define SCOPE_ACT_METRICS 11U
#define SCOPE_ACT_BENCH 8U
#define SCOPE_ACT_BENCH_RESULT 9U

/* ---- CMD 0x34 BRIDGE：主循环级 CDC/串口桥开关（网页面板用）----
 * 关掉之后 main() 不再调 chry_dap_usb2uart_handle()，每轮省下几百个 CPU 周期。
 * 高频 J-Scope 采样时这是端到端速率的瓶颈所在；代价是暂停期间 COM 口与
 * RTT-over-USB 都不可用（采样数据走另一条 bulk IN 0x83，不受影响）。
 *   req_hid[3] = 动作号，req_hid[4] = 参数（SET 时 0/1）
 * 响应形状照抄 0x31/0x32：res_hid[3] = 动作号回显，res_hid[4..7] = 状态字。 */
#define CMD_BRIDGE (0x34)
#define BRIDGE_ACT_STATUS 0U
#define BRIDGE_ACT_SET 1U

/* ---- CMD 0x35 SPI：USB→SPI/QSPI 转发桥（src/spi_bridge/）----
 * 控制面（配置/面板档/状态/使能/复位/中止）走这条 HID；数据面与一切与**线序**有关的
 * 动作走新开的一对 bulk：OUT 0x0B / IN 0x8B。响应形状照抄 0x31/0x32/0x34：
 * res_hid[3] = 动作号回显，res_hid[4..] = 状态字/计数器/配置块。
 * 动作表与帧格式见 src/spi_bridge/spi_bridge_proto.h 与 docs/usb-spi-bridge-plan.md。 */
#define CMD_SPI (0x35)

/* ---- CMD 0x36 I2C：USB→I2C 转发桥（src/i2c_bridge/）----
 * I2C 慢、事务小，所以控制面与数据面都在**同一条 HID 报文**里（不开 bulk、不做 DMA）：
 * 一次 XFER 登记一次事务（主循环执行，最长几毫秒），主机轮询 RESULT 取数据。
 * 响应形状同上：res_hid[3] = 动作号回显，res_hid[4..7] = 状态字，res_hid[8..] = 数据。
 * 动作表/状态字/XFER 布局见 src/i2c_bridge/i2c_bridge_proto.h 与 docs/web-handoff-i2c-bridge.md。 */
#define CMD_I2C (I2C_HID_CMD)

#define PARAM_MAGIC_NUMBER (0x0D000721UL)
/* EasyFlash ENV key that stores the whole api_param_t blob. */
#define API_PARAM_ENV_KEY "cfg"

#define VREF_MV_MIN (1800U)
#define VREF_MV_MAX (5000U)

const api_param_t g_param_default = {
    .magic_number = PARAM_MAGIC_NUMBER,
    .output_mode = 0,
    .usb5v_out_mode = 1, /* level-shifter supply, on by default (no-op boards without one) */
    .clock_accel_mode = 0,
    .led1_mode = LED_MODE_POWER, /* LED1: debugger power, always on */
#if BOARD_HAS_VREF_ADC
    .led2_mode = LED_MODE_VREF,  /* LED2: external reference detection */
#else
    /* No VREF divider on this board: keep LED2 dark. Both LED modes drive
     * the same single LED there, so LED1 keeps priority. */
    .led2_mode = LED_MODE_OFF,
#endif
    .vref_mv = 3300,
};

api_param_t g_param;

static uint16_t clamp_vref(uint16_t v)
{
    if (v < VREF_MV_MIN)
    {
        return VREF_MV_MIN;
    }
    if (v > VREF_MV_MAX)
    {
        return VREF_MV_MAX;
    }
    return v;
}

static uint8_t clamp_led_mode(uint8_t m)
{
    if ((m < LED_MODE_DAP_RUNNING) || (m > LED_MODE_OFF))
    {
        return LED_MODE_OFF;
    }
    return m;
}

void api_param_apply(void)
{
    board_set_5v_output(g_param.usb5v_out_mode ? 1U : 0U);
}

void api_param_load(void)
{
    uint8_t loaded = 0U;

    if (easyflash_init() == EF_NO_ERR)
    {
        size_t saved_len = 0U;
        size_t read_len = ef_get_env_blob(API_PARAM_ENV_KEY, &g_param, sizeof(g_param), &saved_len);
        if ((read_len == sizeof(g_param)) && (g_param.magic_number == PARAM_MAGIC_NUMBER))
        {
            loaded = 1U;
        }
    }

    if (!loaded)
    {
        g_param = g_param_default;
        api_param_save();
    }

    /* Sanitize values coming from flash. */
    g_param.led1_mode = clamp_led_mode(g_param.led1_mode);
    g_param.led2_mode = clamp_led_mode(g_param.led2_mode);
    g_param.vref_mv = clamp_vref(g_param.vref_mv);

    api_param_apply();
}

void api_param_save(void)
{
    if (easyflash_init() != EF_NO_ERR)
    {
        return;
    }
    (void)ef_set_env_blob(API_PARAM_ENV_KEY, &g_param, sizeof(g_param));
}

void api_param_request_save(void)
{
    s_save_pending = 1;
}

void api_param_poll(void)
{
    if (s_save_pending)
    {
        s_save_pending = 0;
        api_param_save();
    }
}

void api_param_proc_hid(uint8_t *req_hid, uint8_t *res_hid)
{
    uint8_t cmd = req_hid[2];
    switch (cmd)
    {
    case CMD_UART_DIAG: {
        uint32_t words[8];
        uartx_get_diag(words);
        res_hid[1] = 33U;
        res_hid[2] = CMD_UART_DIAG;
        memcpy(&res_hid[3], words, sizeof(words));
        break;
    }
    case CMD_GET_CONFIG:
        res_hid[1] = 0x07;
        res_hid[2] = CMD_GET_CONFIG;
        res_hid[3] = g_param.output_mode;
        res_hid[4] = g_param.usb5v_out_mode;
        res_hid[5] = g_param.clock_accel_mode;
        res_hid[6] = g_param.led1_mode;
        res_hid[7] = g_param.led2_mode;
        res_hid[8] = (uint8_t)(g_param.vref_mv & 0xFF);
        res_hid[9] = (uint8_t)((g_param.vref_mv >> 8) & 0xFF);
        break;
    case CMD_SET_CONFIG:
        g_param.output_mode = req_hid[3] ? 0x01 : 0x00;
        g_param.usb5v_out_mode = req_hid[4] ? 0x01 : 0x00;
        g_param.clock_accel_mode = req_hid[5] ? 0x01 : 0x00;
        g_param.led1_mode = clamp_led_mode(req_hid[6]);
        g_param.led2_mode = clamp_led_mode(req_hid[7]);
        g_param.vref_mv = clamp_vref((uint16_t)(req_hid[8] | ((uint16_t)req_hid[9] << 8)));
        api_param_apply();
        res_hid[1] = 1;
        res_hid[2] = CMD_SET_CONFIG;
        break;
    case CMD_GET_VOLTAGE:
    {
        uint16_t vol = led_state_get_external_mv(); /* measured target reference, mV */
        res_hid[1] = 3;
        res_hid[2] = CMD_GET_VOLTAGE;
        res_hid[3] = (vol >> 0) & 0xFF;
        res_hid[4] = (vol >> 8) & 0xFF;
    }
    break;
    case CMD_RTT:
    {
        /* Probe-side RTT bridge control. All SWD work is deferred to the main
         * loop (rtt_bridge_poll): the bit-bang engine must not run in the USB
         * interrupt context.
         * req_hid[3] = action; START takes addr/size/channel from req_hid[4..12];
         * RAW_DAP takes a length + request bytes.
         * Response: res_hid[3] = return code, res_hid[4..] = 12 status words. */
        uint32_t out[12] = {0};
        int8_t rc = 0;

        switch (req_hid[3])
        {
        case RTT_ACT_STOP:
            /* 排队到主循环：收尾要把没落地的 RdOff 补写回去（USB 中断里不能碰 SWD） */
            rtt_bridge_request_stop();
            break;
        case RTT_ACT_START:
        {
            uint32_t addr = (uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                            ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24);
            uint32_t size = (uint32_t)req_hid[8] | ((uint32_t)req_hid[9] << 8) |
                            ((uint32_t)req_hid[10] << 16) | ((uint32_t)req_hid[11] << 24);
            scope_sampler_stop();      /* 采样器与桥都要独占 SWD，只能开一个 */
            rtt_bridge_request_start(addr, size, req_hid[12]);
            break;
        }
        case RTT_ACT_AUTOSTART:
            scope_sampler_stop();
            rtt_bridge_request_start(0U, 0U, 0U);
            break;
        case RTT_ACT_RAW_DAP:
        {
            /* Queue a raw CMSIS-DAP request; it runs from the main loop and the
             * response is read back with RTT_ACT_RAW_RESULT. */
            uint8_t req[24];
            uint32_t n = req_hid[4];

            if (n > sizeof(req))
            {
                n = sizeof(req);
            }
            for (uint32_t i = 0U; i < n; i++)
            {
                req[i] = req_hid[5U + i];
            }
            rtt_bridge_request_raw(req, n);
            break;
        }
        case RTT_ACT_RAW_RESULT:
        {
            uint8_t resp[16];
            uint32_t n = rtt_bridge_raw_result(resp, sizeof(resp));

            rc = (int8_t)n;
            for (uint32_t i = 0U; i < n; i++)
            {
                out[i / 4U] |= ((uint32_t)resp[i]) << ((i % 4U) * 8U);
            }
            break;
        }
        case RTT_ACT_PEEK:
        {
            /* Debug helper: read up to 12 words of the PROBE's own memory.
             * req_hid[4..7] = address, req_hid[8] = word count. */
            uint32_t addr = (uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                            ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24);
            uint32_t n = req_hid[8];

            if (n > 12U)
            {
                n = 12U;
            }
            for (uint32_t i = 0U; i < n; i++)
            {
                out[i] = *(volatile uint32_t *)(addr + i * 4U);
            }
            rc = (int8_t)n;
            break;
        }
        case RTT_ACT_CONFIG:
        {
            /* Runtime tuning: req_hid[4..7] = SWD clock (Hz, 0 = keep),
             * req_hid[8..9] = block bytes (0 = keep), req_hid[10] = flags
             * (bit0 = discard: drain the target ring without feeding the CDC,
             * which measures the raw SWD side), req_hid[11] = clock_delay
             * override (0xFF = use the value Set_Clock_Delay picked). */
            uint32_t hz = (uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                          ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24);
            uint32_t chunk = (uint32_t)req_hid[8] | ((uint32_t)req_hid[9] << 8);

            rtt_bridge_configure(hz, chunk, req_hid[10], req_hid[11]);
            break;
        }
        case RTT_ACT_BENCH:
        {
            /* Pure SWD read benchmark: req_hid[4..7] = target address,
             * req_hid[8..9] = bytes per iteration, req_hid[10..11] = iterations. */
            uint32_t addr = (uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                            ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24);
            uint32_t bytes = (uint32_t)req_hid[8] | ((uint32_t)req_hid[9] << 8);
            uint32_t iters = (uint32_t)req_hid[10] | ((uint32_t)req_hid[11] << 8);

            rtt_bridge_request_bench(addr, bytes, iters);
            break;
        }
        case RTT_ACT_BENCH_RESULT:
        {
            uint32_t bytes = 0U;
            uint32_t ticks = 0U;
            int32_t err = 0;

            if (rtt_bridge_bench_result(&bytes, &ticks, &err))
            {
                out[0] = (uint32_t)err; /* 0 = ok, -1/-2 = SWD 初始化/读失败 */
                out[1] = bytes;
                out[2] = ticks;         /* MCHTMR tick，24 MHz */
                rc = 1;
            }
            break;
        }
        case RTT_ACT_TARGET:
            /* Backend changes are global: never switch a live bridge's next
             * read/RdOff write onto another debug bus. No SWD work in this ISR. */
            if (rtt_bridge_is_running() || scope_sampler_is_running())
            {
                rc = -7;              /* busy: stop the active engine first */
            }
            else
            {
                rtt_bridge_set_target(req_hid[4]);
            }
            break;
        case RTT_ACT_STATUS:
        default:
            break;
        }

        /* PEEK / RAW_RESULT / BENCH_RESULT fill out[] themselves; everything
         * else reports the bridge status words. */
        if ((req_hid[3] != RTT_ACT_PEEK) && (req_hid[3] != RTT_ACT_RAW_RESULT) &&
            (req_hid[3] != RTT_ACT_BENCH_RESULT))
        {
            (void)rtt_bridge_status(out, 12U);
            out[10] = (uint32_t)(int32_t)rtt_bridge_start_result();
        }
        res_hid[1] = 1U + 1U + 4U * 12U;
        res_hid[2] = CMD_RTT;
        res_hid[3] = (uint8_t)rc;
        for (uint32_t i = 0U; i < 12U; i++)
        {
            res_hid[4U + i * 4U + 0U] = (uint8_t)(out[i] >> 0);
            res_hid[4U + i * 4U + 1U] = (uint8_t)(out[i] >> 8);
            res_hid[4U + i * 4U + 2U] = (uint8_t)(out[i] >> 16);
            res_hid[4U + i * 4U + 3U] = (uint8_t)(out[i] >> 24);
        }
    }
    break;
    case CMD_GET_MODEL:
        res_hid[0x01] = 1 + sizeof("akaLinkPro");
        res_hid[0x02] = CMD_GET_MODEL;
        strcpy((char*)&res_hid[0x03], "akaLinkPro");
        break;
    case CMD_GET_SERIAL:
        res_hid[0x01] = 0x0E;
        res_hid[0x02] = CMD_GET_SERIAL;
        strcpy((char*)&res_hid[0x03], serial_number_dynamic);
        break;
    case CMD_GET_HWVER:
        res_hid[0x01] = 0x06;
        res_hid[0x02] = CMD_GET_HWVER;
        strncpy((char*)&res_hid[0x03], (const char*)HARDWARE_VER_STR_ADDR, 4);
        res_hid[0x07] = 0x00;
        break;
    case CMD_GET_FWVER:
        res_hid[0x01] = 0x06;
        res_hid[0x02] = CMD_GET_FWVER;
        strncpy((char*)&res_hid[0x03], (const char*)APPLICATION_VER_STR_ADDR, 4);
        res_hid[0x07] = 0x00;
        break;
    case CMD_GET_BLVER:
        res_hid[0x01] = 0x06;
        res_hid[0x02] = CMD_GET_BLVER;
        strncpy((char*)&res_hid[0x03], (const char*)BOOTLOADER_VER_STR_ADDR, 4);
        res_hid[0x07] = 0x00;
        break;
    case CMD_GET_HW_PROD_DATE:
        res_hid[0x01] = 0x15;
        res_hid[0x02] = CMD_GET_HW_PROD_DATE;
        strncpy((char*)&res_hid[0x03], (const char*)HARDWARE_PROD_TS_STR_ADDR, 19);
        res_hid[0x16] = 0x00;
        break;
    case CMD_GET_FW_COMPILE_DATE:
        res_hid[0x01] = 0x15;
        res_hid[0x02] = CMD_GET_FW_COMPILE_DATE;
        strncpy((char*)&res_hid[0x03], (const char*)APPLICATION_TS_STR_ADDR, 19);
        res_hid[0x16] = 0x00;
        break;
    case CMD_GET_BL_COMPILE_DATE:
        res_hid[0x01] = 0x15;
        res_hid[0x02] = CMD_GET_BL_COMPILE_DATE;
        strncpy((char*)&res_hid[0x03], (const char*)BOOTLOADER_TS_STR_ADDR, 19);
        res_hid[0x16] = 0x00;
        break;
    case CMD_SAVE_CONFIG:
        api_param_request_save();
        res_hid[1] = 0x01;
        res_hid[2] = CMD_SAVE_CONFIG;
        break;
    case CMD_SCOPE:
    {
        /* 探针侧 HSS 采样（J-Scope 波形页）。SWD 只在主循环里碰，这里只登记请求。
         *
         * ⚠️ 字节位置的唯一依据是网页侧（web-serial-rtt-tools/app/scope/）：
         *   · 它 `xfer()` 回来的 res **已经剥掉 Report ID**（`res[1] === cmd`），
         *     所以网页的 res[i] == 本函数的 res_hid[i + 1]；
         *   · 启动码读的是 `signed(res[2])`（view.js:356）→ 写 res_hid[3]；
         *   · 标定结果读的是 `getUint32(3)` 与 `getUint32(7)`（view.js:396）
         *     → 写 res_hid[4] 与 res_hid[8]。 */
        uint32_t out[12] = {0};

        switch (req_hid[3])
        {
        case SCOPE_ACT_STOP:
            scope_sampler_stop();
            break;
        case SCOPE_ACT_START:
            /* 互斥：桥和采样器都要独占 SWD。停桥走排队（主循环里先补写 RdOff），
             * 而采样器的启动本来也是排队的，且 rtt_bridge_poll() 排在
             * scope_sampler_poll() 之前 —— 所以这一拍一定是"桥先停、采样器后启"。 */
            rtt_bridge_request_stop();
            scope_sampler_request_start();
            break;
        case SCOPE_ACT_CLOCK:
            scope_sampler_set_clock((uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                                    ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24));
            break;
        case SCOPE_ACT_CONFIG:
        case SCOPE_ACT_CONFIG_TICKS:
        {
            /* data 段：action(1) period_us(4) flags(1) nvars(1) n×(addr4,size1,type1)
             * 8 个变量 = 55 B，一条 HID 报文（上限 61 B）正好装下。 */
            uint32_t period = (uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                              ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24);
            uint8_t flags = req_hid[8];
            uint8_t n = req_hid[9];
            scope_var_t vars[SCOPE_MAX_VARS];

            if (n > SCOPE_MAX_VARS) { n = SCOPE_MAX_VARS; }
            for (uint8_t i = 0U; i < n; i++)
            {
                const uint8_t *p = &req_hid[10U + (uint16_t)i * 6U];
                vars[i].addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                vars[i].size = p[4];
                vars[i].type = p[5];
                vars[i].rsv = 0U;
            }
            /* 返回码经 res[2]（start_result）与状态字 10 回报：-6 = 变量宽度非法整包拒绝。
             * 被拒时变量表会被清空，主机随后发 START 会拿到 -3，不会拿旧计划偷偷跑。 */
            if (req_hid[3] == SCOPE_ACT_CONFIG_TICKS)
            {
                (void)scope_sampler_configure_ticks(period, flags, n, vars);
            }
            else
            {
                (void)scope_sampler_configure(period, flags, n, vars);
            }
            break;
        }
        case SCOPE_ACT_BENCH:
            scope_sampler_request_bench((uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                                        ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24));
            break;
        case SCOPE_ACT_BENCH_RESULT:
        {
            uint32_t iters = 0U, ticks = 0U;
            int32_t err = 0;

            if (scope_sampler_bench_result(&iters, &ticks, &err) == 0)
            {
                err = -1;                      /* 还没测过 */
            }
            res_hid[1] = 1U + 1U + 12U;
            res_hid[2] = CMD_SCOPE;
            res_hid[3] = 0U;
            memcpy(&res_hid[4], &ticks, 4U);
            memcpy(&res_hid[8], &iters, 4U);
            memcpy(&res_hid[12], &err, 4U);
            /* 标定响应的字 3/4 平时用不到，拿来回报"实际装载了哪个 SWD blob"和
             * clock_delay —— 否则分辨不出"时钟命令被忽略"和"生效了但没差别"。
             * 偏移对照表见 SW_DP.h。 */
            {
                uint32_t blob = swd_blob_read_offset();
                uint32_t cdelay = swd_blob_clock_delay();
                memcpy(&res_hid[16], &blob, 4U);
                memcpy(&res_hid[20], &cdelay, 4U);
            }
            break;
        }
        case SCOPE_ACT_TRIGGER:                    /* v2：探针侧触发，当前只回 OK */
        case SCOPE_ACT_STATUS:
        default:
            break;
        }

        if (req_hid[3] != SCOPE_ACT_BENCH_RESULT)
        {
            if (req_hid[3] == SCOPE_ACT_METRICS)
                (void)scope_sampler_metrics(out, 12U);
            else
                (void)scope_sampler_status(out, 12U);
            res_hid[1] = 1U + 1U + 4U * 12U;
            res_hid[2] = CMD_SCOPE;
            res_hid[3] = (uint8_t)(int8_t)scope_sampler_start_result();   /* 网页在 res[2] 读它 */
            for (uint32_t i = 0U; i < 12U; i++)
            {
                res_hid[4U + i * 4U + 0U] = (uint8_t)(out[i] >> 0);
                res_hid[4U + i * 4U + 1U] = (uint8_t)(out[i] >> 8);
                res_hid[4U + i * 4U + 2U] = (uint8_t)(out[i] >> 16);
                res_hid[4U + i * 4U + 3U] = (uint8_t)(out[i] >> 24);
            }
        }
        break;
    }
    case CMD_RISCV:
    {
        /* Probe-side RISC-V engine (JTAG only). The JTAG bit-bang must not run
         * in the USB interrupt context, so everything is queued here and run by
         * riscv_svc_poll() from the main loop; the reply carries the status
         * block of the *previous* operation (like CMD_RTT does).
         *   req_hid[3] = action, [4..7] = addr, [8..11] = arg1, [12..13] = arg2 */
        uint32_t out[12] = {0};
        uint32_t addr = (uint32_t)req_hid[4] | ((uint32_t)req_hid[5] << 8) |
                        ((uint32_t)req_hid[6] << 16) | ((uint32_t)req_hid[7] << 24);
        uint32_t arg1 = (uint32_t)req_hid[8] | ((uint32_t)req_hid[9] << 8) |
                        ((uint32_t)req_hid[10] << 16) | ((uint32_t)req_hid[11] << 24);
        uint32_t arg2 = (uint32_t)req_hid[12] | ((uint32_t)req_hid[13] << 8);

        /* STATUS and CONFIG are read/modify-only: queueing them would clobber
         * an outstanding operation (the slot is a single word). */
        if (req_hid[3] == RISCV_ACT_STATUS)
        {
            /* nothing to queue: just report */
        }
        else if (req_hid[3] == RISCV_ACT_CONFIG)
        {
            riscv_svc_set_delay(addr);
        }
        else
        {
            riscv_svc_request(req_hid[3], addr, arg1, arg2);
        }
        (void)riscv_svc_status(out, 12U);

        res_hid[1] = 1U + 1U + 4U * 12U;
        res_hid[2] = CMD_RISCV;
        res_hid[3] = (uint8_t)req_hid[3];
        for (uint32_t i = 0U; i < 12U; i++)
        {
            res_hid[4U + i * 4U + 0U] = (uint8_t)(out[i] >> 0);
            res_hid[4U + i * 4U + 1U] = (uint8_t)(out[i] >> 8);
            res_hid[4U + i * 4U + 2U] = (uint8_t)(out[i] >> 16);
            res_hid[4U + i * 4U + 3U] = (uint8_t)(out[i] >> 24);
        }
        break;
    }
    case CMD_BRIDGE:
    {
        /* status[0] = bit0 当前开关 / bit8 = 该命令是否被支持（恒 1，给网页探活） */
        uint32_t st = (uint32_t)chry_dap_usb2uart_is_enabled() | (1UL << 8);

        if (req_hid[3] == BRIDGE_ACT_SET)
        {
            chry_dap_usb2uart_set_enabled(req_hid[4] ? 1U : 0U);
            st = (uint32_t)chry_dap_usb2uart_is_enabled() | (1UL << 8);
        }

        res_hid[1] = 1U + 1U + 4U;
        res_hid[2] = CMD_BRIDGE;
        res_hid[3] = req_hid[3];
        res_hid[4] = (uint8_t)(st >> 0);
        res_hid[5] = (uint8_t)(st >> 8);
        res_hid[6] = (uint8_t)(st >> 16);
        res_hid[7] = (uint8_t)(st >> 24);
        break;
    }
    case CMD_SPI:
    {
        /* USB→SPI/QSPI 转发桥的控制面（动作见 src/spi_bridge/spi_bridge_proto.h）。
         * 数据面走新开的一对 bulk：OUT 0x0B / IN 0x8B。 */
        spi_bridge_hid(req_hid, res_hid);
        break;
    }
    case SPI_CDC_CMD:
        spi_cdc_hid(req_hid, res_hid);
        break;
    case CMD_I2C:
    {
        /* USB→I2C 转发桥（动作见 src/i2c_bridge/i2c_bridge_proto.h）。
         * 控制面与数据面都在这一条 HID 上：中断里只登记请求，主循环执行事务。 */
        i2c_bridge_hid(req_hid, res_hid);
        break;
    }

    case ANALOG_CMD:
        analog_bridge_hid(req_hid, res_hid);
        break;
    case BP_CMD:
        bus_periodic_hid(req_hid, res_hid);
        break;
    case CMD_RESET_DEVICE:
        ppor_reset_mask_set_source_enable(HPM_PPOR, ppor_reset_software);
        ppor_sw_reset(HPM_PPOR, 24);
        break;
    case CMD_ENTER_DFU:
        hpm_dfu_reboot_to_dfu();
        break;
    default:
        res_hid[1] = 0x01;
        res_hid[2] = CMD_NOT_SUPPORT;
        break;
    }
}
