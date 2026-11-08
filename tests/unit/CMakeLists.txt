cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trainer_unit)

# Pure logic modules from the application, tested without any hardware.
set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

target_include_directories(app PRIVATE
	src
	${APP_SRC}/trigger
	${APP_SRC}/weapon
	${APP_SRC}/proto
	${APP_SRC}/cfg
	${APP_SRC}/imu
	${APP_SRC}/timesync
	${APP_SRC}/strobe
)

target_sources(app PRIVATE
	src/test_rounds.c
	src/test_trigger.c
	src/test_weapon.c
	src/test_proto.c
	src/test_cfg.c
	src/test_imu.c
	src/test_malfunction.c
	src/test_timesync.c
	src/test_shotlog.c
	src/test_strobe.c
	${APP_SRC}/trigger/trigger_proc.c
	${APP_SRC}/weapon/rounds.c
	${APP_SRC}/weapon/weapon_sm.c
	${APP_SRC}/weapon/malfunction.c
	${APP_SRC}/weapon/shotlog.c
	${APP_SRC}/proto/proto.c
	${APP_SRC}/cfg/cfg.c
	${APP_SRC}/imu/imu_metrics.c
	${APP_SRC}/timesync/timesync.c
	${APP_SRC}/strobe/strobe.c
)
