SHELL := bash
.ONESHELL:

# Benchmark hosts (placeholders) — override with your machines, e.g.
# `make latency-vs-size HOST1=... HOST2=... HOST3=...`.
HOST1        := node1
HOST2        := node2
HOST3        := node3
PULSAR_PORT       := 6650
PULSAR_ADMIN_PORT := 8080
PULSAR_URL        := pulsar://$(HOST2):$(PULSAR_PORT)
PULSAR_ADMIN_URL  := http://$(HOST2):$(PULSAR_ADMIN_PORT)

# Pulsar benchmark configuration.
TRANSPORT  := persistent

# rtt send mode for the latency sweeps (MODE=sync|async, default sync). It
# selects the result filename tag: the bare transport for sync, and
# <transport>-async for async.
RTT_MODE = $(if $(MODE),$(MODE),sync)
RTT_TAG  = $(if $(filter async,$(RTT_MODE)),$(TRANSPORT)-async,$(TRANSPORT))

REQ_TOPIC  := rb-rtt-req
RESP_TOPIC := rb-rtt-resp
TPUT_TOPIC := rb-tput

SSH_CONTROL_DIR := ./sockets
SSH_OPTS := -o ControlMaster=auto -o ControlPath=$(SSH_CONTROL_DIR)/%r@%h -o ControlPersist=600
SSH      := ssh $(SSH_OPTS)

REMOTE_PULSAR  := ~/pulsar
REMOTE_RESULTS := ~/results

LOCAL_RESULTS := ./results

$(LOCAL_RESULTS)/rtt_%.csv: | $(LOCAL_RESULTS)
	@TARGET=$$(basename $@)
	TAG=$$(echo "$$TARGET" | sed -n 's/^rtt_\([a-z-]*\)_s.*/\1/p')
	case "$$TAG" in *-async) MODE=async; TRANSPORT=$${TAG%-async} ;; *) MODE=sync; TRANSPORT=$$TAG ;; esac
	SIZE=$$(echo "$$TARGET" | sed -n 's/.*_s\([0-9]*\)_.*/\1/p')
	SEND_CORES=$$(echo "$$TARGET" | sed -n 's/.*_send\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	SEND_CONC=$$(echo "$$TARGET" | sed -n 's/.*_send[0-9+-]*c\([0-9]*\)_.*/\1/p')
	SRV_CORES=$$(echo "$$TARGET" | sed -n 's/.*_srv\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	SRV_CONC=$$(echo "$$TARGET" | sed -n 's/.*_srv[0-9+-]*c\([0-9]*\)_.*/\1/p')
	RECV_CORES=$$(echo "$$TARGET" | sed -n 's/.*_recv\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	RECV_CONC=$$(echo "$$TARGET" | sed -n 's/.*_recv[0-9+-]*c\([0-9]*\)_.*/\1/p')
	DUR=$$(echo "$$TARGET" | sed -n 's/.*_d\([0-9a-z]*\)_l.*/\1/p')
	LOAD=$$(echo "$$TARGET" | sed -n 's/.*_l\([0-9.]*\)\.csv/\1/p')
	mkdir -p $(SSH_CONTROL_DIR)
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/rtt.just init \
	    $(PULSAR_URL) $(PULSAR_ADMIN_URL) $$TRANSPORT $$SIZE $(REQ_TOPIC) $(RESP_TOPIC)" || { echo "init failed"; exit 1; }
	$(SSH) $(HOST3) "true"
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/rtt.just receive \
	    '$$RECV_CORES' $(PULSAR_URL) $$TRANSPORT $$SIZE $(RESP_TOPIC) $$RECV_CONC $$DUR $(REMOTE_RESULTS)" &
	RECV_PID=$$!
	$(SSH) $(HOST3) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/rtt.just server \
	    '$$SRV_CORES' $(PULSAR_URL) $$TRANSPORT $$SIZE $(REQ_TOPIC) $(RESP_TOPIC) $$SRV_CONC $$DUR $$MODE" &
	SERVER_PID=$$!
	sleep 0.5
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/rtt.just send \
	    '$$SEND_CORES' $(PULSAR_URL) $$TRANSPORT $$SIZE $(REQ_TOPIC) $$SEND_CONC $$DUR $$LOAD $$MODE" || { echo "sender failed"; exit 1; }
	wait $$RECV_PID || true
	wait $$SERVER_PID || { echo "server failed"; exit 1; }
	$(SSH) $(HOST1) "mv $(REMOTE_RESULTS)/rtt_$${TRANSPORT}_s$${SIZE}_c$${RECV_CONC}_d$${DUR}_r0.csv \
	    $(REMOTE_RESULTS)/$$TARGET" || { echo "result file missing"; exit 1; }
	rsync -zvhP -e "ssh $(SSH_OPTS)" $(HOST1):$(REMOTE_RESULTS)/$$TARGET $@

$(LOCAL_RESULTS)/throughput_%.csv: | $(LOCAL_RESULTS)
	@TARGET=$$(basename $@)
	TM=$$(echo "$$TARGET" | sed -n 's/^throughput_\([a-z-]*\)_s.*/\1/p')
	MODE=$${TM##*-}; TRANSPORT=$${TM%-*}
	SIZE=$$(echo "$$TARGET" | sed -n 's/.*_s\([0-9]*\)_.*/\1/p')
	PROD_CORES=$$(echo "$$TARGET" | sed -n 's/.*_prod\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	PROD_CONC=$$(echo "$$TARGET" | sed -n 's/.*_prod[0-9+-]*c\([0-9]*\)_.*/\1/p')
	CONS_CORES=$$(echo "$$TARGET" | sed -n 's/.*_cons\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	CONS_CONC=$$(echo "$$TARGET" | sed -n 's/.*_cons[0-9+-]*c\([0-9]*\)_.*/\1/p')
	BATCHES=$$(echo "$$TARGET" | sed -n 's/.*_b\([0-9]*\)_bs.*/\1/p')
	BSIZE=$$(echo "$$TARGET" | sed -n 's/.*_bs\([0-9]*\)_d.*/\1/p')
	DUR=$$(echo "$$TARGET" | sed -n 's/.*_d\([0-9a-z]*\)\.csv/\1/p')
	PROD_COUNT=$$(echo "$$PROD_CORES" | tr -cd ';' | wc -c); PROD_COUNT=$$((PROD_COUNT + 1))
	mkdir -p $(SSH_CONTROL_DIR)
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/throughput.just init \
	    $(PULSAR_URL) $(PULSAR_ADMIN_URL) $$TRANSPORT $$SIZE $(TPUT_TOPIC)" || { echo "init failed"; exit 1; }
	$(SSH) $(HOST3) "true"
	$(SSH) $(HOST3) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/throughput.just consume \
	    '$$CONS_CORES' $(PULSAR_URL) $$TRANSPORT $$SIZE $(TPUT_TOPIC) $$CONS_CONC $$DUR" &
	CONSUMER_PID=$$!
	sleep 0.5
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_PULSAR) && just -f just/throughput.just produce \
	    '$$PROD_CORES' $(PULSAR_URL) $$TRANSPORT $$SIZE $(TPUT_TOPIC) $$PROD_CONC $$BATCHES $$BSIZE $(REMOTE_RESULTS) 1s $$MODE" || { echo "producer failed"; exit 1; }
	wait $$CONSUMER_PID || true
	for id in $$(seq 0 $$((PROD_COUNT - 1))); do \
		BASE=$${TARGET%.csv}; \
		$(SSH) $(HOST1) "mv $(REMOTE_RESULTS)/throughput_$${TRANSPORT}-$${MODE}_s$${SIZE}_b$${BATCHES}_bs$${BSIZE}_id$${id}.csv \
		    $(REMOTE_RESULTS)/$${BASE}_id$${id}.csv" || { echo "result file missing"; exit 1; }; \
		rsync -zvhP -e "ssh $(SSH_OPTS)" $(HOST1):$(REMOTE_RESULTS)/$${BASE}_id$${id}.csv $(LOCAL_RESULTS)/; \
	done
	touch $@

$(LOCAL_RESULTS):
	mkdir -p $@

.PHONY: help latency-vs-size throughput-vs-size

latency-vs-size:
	@if [ -z "$(SIZES)" ]; then echo "Error: SIZES not set"; exit 1; fi
	case "$(RTT_MODE)" in sync|async) ;; *) echo "Error: MODE must be sync or async (got: $(RTT_MODE))"; exit 1 ;; esac
	if [ -z "$(SEND_CORES)" ]; then echo "Error: SEND_CORES not set"; exit 1; fi
	if [ -z "$(SEND_CONC)" ]; then echo "Error: SEND_CONC not set"; exit 1; fi
	if [ -z "$(SRV_CORES)" ]; then echo "Error: SRV_CORES not set"; exit 1; fi
	if [ -z "$(SRV_CONC)" ]; then echo "Error: SRV_CONC not set"; exit 1; fi
	if [ -z "$(RECV_CORES)" ]; then echo "Error: RECV_CORES not set"; exit 1; fi
	if [ -z "$(RECV_CONC)" ]; then echo "Error: RECV_CONC not set"; exit 1; fi
	if [ -z "$(TPUT)" ]; then echo "Error: TPUT not set"; exit 1; fi
	if [ -z "$(DUR)" ]; then echo "Error: DUR not set"; exit 1; fi
	for SIZE in $(SIZES); do \
		$(MAKE) -f $(MAKEFILE_LIST) $(LOCAL_RESULTS)/rtt_$(RTT_TAG)_s$${SIZE}_send$(SEND_CORES)c$(SEND_CONC)_srv$(SRV_CORES)c$(SRV_CONC)_recv$(RECV_CORES)c$(RECV_CONC)_d$(DUR)_l$(TPUT).csv; \
	done

throughput-vs-size:
	@if [ -z "$(SIZES)" ]; then echo "Error: SIZES not set"; exit 1; fi
	if [ -z "$(MODE)" ]; then echo "Error: MODE not set (sync|async)"; exit 1; fi
	if [ -z "$(PROD_CORES)" ]; then echo "Error: PROD_CORES not set"; exit 1; fi
	if [ -z "$(PROD_CONC)" ]; then echo "Error: PROD_CONC not set"; exit 1; fi
	if [ -z "$(CONS_CORES)" ]; then echo "Error: CONS_CORES not set"; exit 1; fi
	if [ -z "$(CONS_CONC)" ]; then echo "Error: CONS_CONC not set"; exit 1; fi
	if [ -z "$(BATCHES)" ]; then echo "Error: BATCHES not set"; exit 1; fi
	if [ -z "$(BSIZE)" ]; then echo "Error: BSIZE not set"; exit 1; fi
	if [ -z "$(DUR)" ]; then echo "Error: DUR not set"; exit 1; fi
	for SIZE in $(SIZES); do \
		$(MAKE) -f $(MAKEFILE_LIST) $(LOCAL_RESULTS)/throughput_$(TRANSPORT)-$(MODE)_s$${SIZE}_prod$(PROD_CORES)c$(PROD_CONC)_cons$(CONS_CORES)c$(CONS_CONC)_b$(BATCHES)_bs$(BSIZE)_d$(DUR).csv; \
	done

help:
	@echo "C++ / pulsar-client-cpp benchmark harness."
	@echo ""
	@echo "Variables:"
	@echo "  HOST1            = $(HOST1)       (sender + receiver / producer host)"
	@echo "  HOST2            = $(HOST2)       (Pulsar broker host)"
	@echo "  HOST3            = $(HOST3)       (echo server / consumer host)"
	@echo "  PULSAR_URL       = $(PULSAR_URL)"
	@echo "  PULSAR_ADMIN_URL = $(PULSAR_ADMIN_URL)  (admin REST API, used by init)"
	@echo "  TRANSPORT        = $(TRANSPORT)   (persistent | non-persistent)"
	@echo "  REMOTE_PULSAR    = $(REMOTE_PULSAR)  (remote checkout path)"
	@echo ""
	@echo "Result filename tags: rtt uses the bare transport for sync sends"
	@echo "(rtt_persistent_..., rtt_non-persistent_...) and <transport>-async for"
	@echo "async sends (rtt_persistent-async_..., rtt_non-persistent-async_...);"
	@echo "throughput always appends the send mode (throughput_persistent-sync_...,"
	@echo "throughput_non-persistent-async_...)."
	@echo ""
	@echo "Sweep targets:"
	@echo "  make latency-vs-size       SIZES=... SEND_CORES=... SEND_CONC=... SRV_CORES=... SRV_CONC=... RECV_CORES=... RECV_CONC=... TPUT=... DUR=... [MODE=sync|async]"
	@echo "  make throughput-vs-size    SIZES=... MODE=sync|async PROD_CORES=... PROD_CONC=... CONS_CORES=... CONS_CONC=... BATCHES=... BSIZE=... DUR=..."
