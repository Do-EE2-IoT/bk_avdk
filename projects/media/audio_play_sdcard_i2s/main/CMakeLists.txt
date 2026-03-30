set(incs .
		include
	)

set(srcs
		app_main.c
		app_ws.c
		ota_github.c
		tas_5711.c
	)

if (CONFIG_SYS_CPU0)
	list(APPEND srcs
		audio_play.c
	)
endif()

set(priv_req bk_init bk_cli avdk_libs media_service ota)

list(APPEND priv_req multimedia bk_websocket bk_event)

if (CONFIG_OVERRIDE_FLASH_PARTITION)
	list(APPEND srcs
		vendor_flash.c
	)
endif()

armino_component_register(
	SRCS "${srcs}"
	INCLUDE_DIRS "${incs}"
	PRIV_REQUIRES "${priv_req}" bk_common bk_netif bk_ps fatfs bk_init at bk_wifi compal http bk_https mbedtls tfm psa_mbedtls
)
