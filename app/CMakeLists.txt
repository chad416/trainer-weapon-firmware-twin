cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(trainer_fw LANGUAGES C)

target_include_directories(app PRIVATE
	src
	src/trigger
	src/weapon
	src/io
	src/control
	src/proto
	src/cfg
	src/can
	src/sys
	src/imu
	src/timesync
)

target_sources(app PRIVATE
	src/main.c
	src/trigger/trigger_proc.c
	src/trigger/trigger_task.c
	src/weapon/rounds.c
	src/weapon/weapon_sm.c
	src/weapon/malfunction.c
	src/weapon/shotlog.c
	src/io/outputs.c
	src/io/inputs.c
	src/control/control.c
	src/proto/proto.c
	src/cfg/cfg.c
	src/cfg/cfg_store.c
	src/can/canlink.c
	src/can/lane.c
	src/sys/wdt.c
	src/sys/bit.c
	src/imu/imu_metrics.c
	src/imu/imu_task.c
	src/timesync/timesync.c
	src/shell/shell_cmds.c
)
