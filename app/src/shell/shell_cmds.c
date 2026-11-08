/* Diagnostic shell: `trainer status|inject|version|cfg|bit|can|stall`. */
#include <stdlib.h>
#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/shell/shell.h>

#include "bit.h"
#include "canlink.h"
#include "cfg_store.h"
#include "control.h"
#include "imu_task.h"
#include "lane.h"
#include "strobe_task.h"
#include "trigger_task.h"
#include "wdt.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct control_snapshot c;
	struct trigger_snapshot t;

	control_get_snapshot(&c);
	trigger_get_snapshot(&t);

	shell_print(sh, "state=%s sel=%s mag=%s mag_rounds=%u chambered=%s total=%u shots=%u",
		    weapon_state_name(c.state), weapon_selector_name(c.sel),
		    c.rounds.mag_in ? "in" : "out", c.rounds.mag_rounds,
		    c.rounds.chambered ? "yes" : "no", rounds_total(&c.rounds), c.shots);
	shell_print(sh,
		    "trigger raw=%d travel=%u/1000 armed=%s samples=%u adc_err=%u read_max=%uus "
		    "uptime=%ums",
		    t.raw, t.travel_pm, t.armed ? "yes" : "no", t.samples, t.adc_errors,
		    t.read_us_max, k_uptime_get_32());
	shell_print(sh, "last_break takeup=%ums squeeze=%ums jerk=%u/ms dropped_events=%u",
		    t.last.takeup_ms, t.last.squeeze_ms, t.last.jerk_pm_per_ms, c.dropped_events);
	shell_print(sh, "node=%u boot=%u reset=%s faults=0x%04x", cfg_store_node_id(),
		    cfg_store_boot_count(), wdt_reset_reason_name(wdt_reset_reason()),
		    bit_last_mask());
	return 0;
}

static int cmd_inject(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (control_post(WEV_STOPPAGE_INJECT) != 0) {
		shell_error(sh, "event queue full");
		return -ENOMSG;
	}
	shell_print(sh, "stoppage injected");
	return 0;
}

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "trainer_fw %s", APP_VERSION_STRING);
	return 0;
}

static int cmd_cfg_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct trainer_cfg c;

	cfg_store_get(&c);
	shell_print(sh,
		    "cfg node_id=%u capacity=%u auto_rpm=%u emitter_us=%u valve_ms=%u break_pm=%u "
		    "reset_pm=%u",
		    c.node_id, c.capacity, c.auto_rpm, c.emitter_us, c.valve_ms, c.break_pm,
		    c.reset_pm);
	return 0;
}

static int cmd_cfg_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint8_t key = (uint8_t)strtoul(argv[1], NULL, 0);
	uint32_t value = (uint32_t)strtoul(argv[2], NULL, 0);
	uint8_t status = cfg_store_set(key, value);

	shell_print(sh, "cfg set key=%u value=%u status=%u", key, value, status);
	return (status == 0U) ? 0 : -EINVAL;
}

#if defined(CONFIG_TRAINER_DEBUG_CMDS)
static int cmd_cfg_corrupt(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = cfg_store_corrupt();

	shell_print(sh, "cfg corrupted (rc=%d), takes effect at the next boot", rc);
	return rc;
}
#endif

static int cmd_bit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint16_t mask = bit_run();

	shell_print(sh, "bit mask=0x%04x critical=%s", mask, bit_is_critical(mask) ? "yes" : "no");
	return 0;
}

static int cmd_can(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct canlink_stats s;

	canlink_get_stats(&s);
	shell_print(sh,
		    "can up=%s state=%d tx_ok=%u tx_err=%u tx_overflow=%u bus_off=%u rx_cmd=%u "
		    "rx_drop=%u rx_sync=%u",
		    s.up ? "yes" : "no", (int)s.state, s.tx_ok, s.tx_errors, s.tx_overflows,
		    s.bus_off_count, s.rx_cmds, s.rx_dropped, s.rx_syncs);
	return 0;
}

/* `trainer log [n|clear]`: the last n shots (default 10), oldest first. SPEC F15. */
static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t count;
	uint32_t total;
	unsigned long n = 10UL;

	if ((argc > 1) && (strcmp(argv[1], "clear") == 0)) {
		control_shotlog_clear();
		shell_print(sh, "log cleared");
		return 0;
	}
	if (argc > 1) {
		char *end;

		n = strtoul(argv[1], &end, 0);
		if ((*end != '\0') || (n == 0UL)) {
			shell_error(sh, "usage: trainer log [n|clear]");
			return -EINVAL;
		}
	}

	control_shotlog_stats(&count, &total);
	n = MIN(n, (unsigned long)count);
	shell_print(sh, "log count=%u total=%u showing=%lu capacity=%u", count, total, n,
		    SHOTLOG_CAPACITY);

	for (unsigned long age = n; age-- > 0UL;) {
		struct shot_record r;

		if (!control_shotlog_get((uint16_t)age, &r)) {
			break;
		}
		shell_print(sh,
			    "log #%u t_lane=%u mode=%s left=%u takeup=%ums squeeze=%ums jerk=%u "
			    "rate=%u cant=%d flinch_ddeg=%d",
			    r.shot_no, r.t_lane_us, weapon_selector_name((enum weapon_selector)r.mode),
			    r.rounds_left, r.takeup_4ms * 4U, r.squeeze_4ms * 4U, r.jerk_pm_per_ms,
			    r.muzzle_rate_dps, r.cant_deg, r.flinch_ddeg);
	}
	return 0;
}

static int cmd_imu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct imu_sample s;
	struct imu_shot_metrics m;

	if (!imu_latest(&s)) {
		shell_error(sh, "no IMU samples");
		return -ENODATA;
	}
	(void)imu_shot_metrics(s.t_us, &m);
	/* milli-units: no float printf support needed */
	shell_print(sh, "imu gyro_mdps=%d,%d,%d acc_mg=%d,%d,%d", (int)(s.gx * 1000.0f),
		    (int)(s.gy * 1000.0f), (int)(s.gz * 1000.0f), (int)(s.ax * 1000.0f),
		    (int)(s.ay * 1000.0f), (int)(s.az * 1000.0f));
	shell_print(sh, "imu now rate_mdps=%d cant_mdeg=%d flinch_mdeg=%d samples=%u",
		    (int)(m.muzzle_rate_dps * 1000.0f), (int)(m.cant_deg * 1000.0f),
		    (int)(m.flinch_deg * 1000.0f), m.samples);
	return 0;
}

static int cmd_sync(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct lane_sync_info i;

	lane_get_sync_info(&i);
	shell_print(sh, "sync synced=%s offset_us=%lld drift_ppb=%d updates=%u resets=%u",
		    i.synced ? "yes" : "no", (long long)i.offset_us, i.drift_ppb, i.updates,
		    i.resets);
	return 0;
}

static int cmd_strobe(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct strobe_stats s;

	strobe_get_stats(&s);
	shell_print(sh,
		    "strobe slots=%u slot=%u synced=%s pulses=%u aim=%u shots=%u late=%u last_kind=%u "
		    "last_k=%lld last_lane=%lld last_err_us=%lld max_err_us=%lld shot_k=%lld "
		    "shot_fired_k=%lld shot_lane=%lld",
		    s.n_slots, s.slot, s.synced ? "yes" : "no", s.pulses, s.aim, s.shots, s.late,
		    s.last_kind, (long long)s.last_k, (long long)s.last_lane_us,
		    (long long)s.last_err_us, (long long)s.max_err_us, (long long)s.last_shot_k,
		    (long long)s.last_shot_fired_k, (long long)s.last_shot_lane_us);
	return 0;
}

#if defined(CONFIG_TRAINER_DEBUG_CMDS)
static int cmd_stall(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "control") == 0) {
		control_debug_stall();
	} else if (strcmp(argv[1], "trigger") == 0) {
		trigger_debug_stall();
	} else {
		shell_error(sh, "usage: trainer stall <control|trigger>");
		return -EINVAL;
	}
	shell_print(sh, "stalling %s: the task watchdog should reset the MCU", argv[1]);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(cfg_cmds,
	SHELL_CMD_ARG(show, NULL, "Show the configuration", cmd_cfg_show, 1, 0),
	SHELL_CMD_ARG(set, NULL, "Set <key> <value> (see protocol/trainer.dbc)", cmd_cfg_set, 3, 0),
#if defined(CONFIG_TRAINER_DEBUG_CMDS)
	SHELL_CMD_ARG(corrupt, NULL, "Debug: store a config with a bad CRC", cmd_cfg_corrupt, 1, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(trainer_cmds,
	SHELL_CMD(status, NULL, "Weapon, trigger and system status", cmd_status),
	SHELL_CMD(inject, NULL, "Inject a stoppage (instructor command)", cmd_inject),
	SHELL_CMD(version, NULL, "Firmware version", cmd_version),
	SHELL_CMD(cfg, &cfg_cmds, "Configuration", NULL),
	SHELL_CMD(bit, NULL, "Run the built-in self-test", cmd_bit),
	SHELL_CMD(can, NULL, "CAN link statistics", cmd_can),
	SHELL_CMD_ARG(log, NULL, "Shot log: last n shots (default 10), or 'clear'", cmd_log, 1, 1),
	SHELL_CMD(imu, NULL, "Latest IMU sample and current metrics", cmd_imu),
	SHELL_CMD(sync, NULL, "Lane time synchronisation", cmd_sync),
	SHELL_CMD(strobe, NULL, "Emitter strobing for the lane camera (v0.6)", cmd_strobe),
#if defined(CONFIG_TRAINER_DEBUG_CMDS)
	SHELL_CMD_ARG(stall, NULL, "Debug: stall <control|trigger> to test the watchdog",
		      cmd_stall, 2, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(trainer, &trainer_cmds, "Trainer weapon commands", NULL);
