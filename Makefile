obj-$(CONFIG_NTRDMA)    += ntrdma.o

ntrdma-y	+= ntrdma_util.o
ntrdma-y	+= ntrdma_dev.o
ntrdma-y	+= ntrdma_ib.o
ntrdma-y	+= ntrdma_vbell.o
ntrdma-y	+= ntrdma_res.o
ntrdma-y	+= ntrdma_cmd.o
ntrdma-y	+= ntrdma_cq.o
ntrdma-y	+= ntrdma_pd.o
ntrdma-y	+= ntrdma_mr.o
ntrdma-y	+= ntrdma_qp.o
ntrdma-y	+= ntrdma_zip.o
ntrdma-y	+= ntrdma_eth.o

ifdef CONFIG_NTRDMA_DEBUGFS
ntrdma-y	+= ntrdma_debugfs.o
endif

ifdef CONFIG_NTRDMA_TCP
ntrdma-y	+= ntrdma_tcp.o
endif

ifdef CONFIG_NTRDMA_NTB
ntrdma-y	+= ntrdma_os_linux.o
ntrdma-y	+= ntrdma_ntb.o
ntrdma-y	+= ntrdma_dma.o
ntrdma-y	+= ntrdma_ping.o
ntrdma-y	+= ntrdma_port.o
ntrdma-y	+= ntrdma_port_v1.o
endif

