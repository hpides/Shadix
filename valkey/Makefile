SHELL := bash
.ONESHELL:

# Benchmark hosts (placeholders) — override with your machines, e.g.
# `make latency-vs-size HOST1=... HOST2=... HOST3=...`.
HOST1        := node1
HOST2        := node2
HOST3        := node3
VALKEY_PORT  := 6379
# Default scheme: RDMA. Override to redis://host:port for TCP.
VALKEY_URL   := rdma://$(HOST2):$(VALKEY_PORT)

# Valkey benchmark configuration.
TRANSPORT  := list
# Send mode for the sweep targets: sync (default) or async. sync keeps the
# bare transport tag in result filenames (the published throughput_list_...
# / rtt_list_... files); async appends -async (list-async).
MODE       ?= sync
TAG        := $(TRANSPORT)$(if $(filter async,$(MODE)),-async)

REQ_KEY    := rb:rtt:req
RESP_KEY   := rb:rtt:resp
TPUT_KEY   := rb:tput

SSH_CONTROL_DIR := ./sockets
SSH_OPTS := -o ControlMaster=auto -o ControlPath=$(SSH_CONTROL_DIR)/%r@%h -o ControlPersist=600
SSH      := ssh $(SSH_OPTS)

REMOTE_VALKEY    := ~/valkey
REMOTE_RESULTS := ~/results

LOCAL_RESULTS := ./results

$(LOCAL_RESULTS)/rtt_%.csv: | $(LOCAL_RESULTS)
	@TARGET=$$(basename $@)
	TAG=$$(echo "$$TARGET" | sed -n 's/^rtt_\([a-z-]*\)_s.*/\1/p')
	# Split only on the -async suffix: transport names may contain '-' too.
	case "$$TAG" in
		*-async) MODE=async; TRANSPORT=$${TAG%-async} ;;
		*)       MODE=sync;  TRANSPORT=$$TAG ;;
	esac
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
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/rtt.just init \
	    $(VALKEY_URL) $$TRANSPORT $$SIZE $(REQ_KEY) $(RESP_KEY)" || { echo "init failed"; exit 1; }
	$(SSH) $(HOST3) "true"
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/rtt.just receive \
	    '$$RECV_CORES' $(VALKEY_URL) $$TRANSPORT $$SIZE $(RESP_KEY) $$RECV_CONC $$DUR $(REMOTE_RESULTS)" &
	RECV_PID=$$!
	$(SSH) $(HOST3) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/rtt.just server \
	    '$$SRV_CORES' $(VALKEY_URL) $$TRANSPORT $$SIZE $(REQ_KEY) $(RESP_KEY) $$SRV_CONC $$DUR $$MODE" &
	SERVER_PID=$$!
	sleep 0.5
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/rtt.just send \
	    '$$SEND_CORES' $(VALKEY_URL) $$TRANSPORT $$SIZE $(REQ_KEY) $$SEND_CONC $$DUR $$LOAD $$MODE" || { echo "sender failed"; exit 1; }
	wait $$RECV_PID || true
	wait $$SERVER_PID || { echo "server failed"; exit 1; }
	$(SSH) $(HOST1) "mv $(REMOTE_RESULTS)/rtt_$${TRANSPORT}_s$${SIZE}_c$${RECV_CONC}_d$${DUR}_r0.csv \
	    $(REMOTE_RESULTS)/$$TARGET" || { echo "result file missing"; exit 1; }
	rsync -zvhP -e "ssh $(SSH_OPTS)" $(HOST1):$(REMOTE_RESULTS)/$$TARGET $@

$(LOCAL_RESULTS)/throughput_%.csv: | $(LOCAL_RESULTS)
	@TARGET=$$(basename $@)
	TAG=$$(echo "$$TARGET" | sed -n 's/^throughput_\([a-z-]*\)_s.*/\1/p')
	# Split only on the -async suffix: transport names may contain '-' too.
	case "$$TAG" in
		*-async) MODE=async; TRANSPORT=$${TAG%-async} ;;
		*)       MODE=sync;  TRANSPORT=$$TAG ;;
	esac
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
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/throughput.just init \
	    $(VALKEY_URL) $$TRANSPORT $$SIZE $(TPUT_KEY)" || { echo "init failed"; exit 1; }
	$(SSH) $(HOST3) "true"
	$(SSH) $(HOST3) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/throughput.just consume \
	    '$$CONS_CORES' $(VALKEY_URL) $$TRANSPORT $$SIZE $(TPUT_KEY) $$CONS_CONC $$DUR" &
	CONSUMER_PID=$$!
	sleep 0.5
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_VALKEY) && just -f just/throughput.just produce \
	    '$$PROD_CORES' $(VALKEY_URL) $$TRANSPORT $$SIZE $(TPUT_KEY) $$PROD_CONC $$BATCHES $$BSIZE $(REMOTE_RESULTS) 1s $$MODE" || { echo "producer failed"; exit 1; }
	wait $$CONSUMER_PID || true
	for id in $$(seq 0 $$((PROD_COUNT - 1))); do \
		BASE=$${TARGET%.csv}; \
		$(SSH) $(HOST1) "mv $(REMOTE_RESULTS)/throughput_$${TAG}_s$${SIZE}_b$${BATCHES}_bs$${BSIZE}_id$${id}.csv \
		    $(REMOTE_RESULTS)/$${BASE}_id$${id}.csv" || { echo "result file missing"; exit 1; }; \
		rsync -zvhP -e "ssh $(SSH_OPTS)" $(HOST1):$(REMOTE_RESULTS)/$${BASE}_id$${id}.csv $(LOCAL_RESULTS)/; \
	done
	touch $@

$(LOCAL_RESULTS):
	mkdir -p $@

.PHONY: help latency-vs-size throughput-vs-size

latency-vs-size:
	@if [ -z "$(SIZES)" ]; then echo "Error: SIZES not set"; exit 1; fi
	if [ "$(MODE)" != "sync" ] && [ "$(MODE)" != "async" ]; then echo "Error: MODE must be sync or async (got '$(MODE)')"; exit 1; fi
	if [ -z "$(SEND_CORES)" ]; then echo "Error: SEND_CORES not set"; exit 1; fi
	if [ -z "$(SEND_CONC)" ]; then echo "Error: SEND_CONC not set"; exit 1; fi
	if [ -z "$(SRV_CORES)" ]; then echo "Error: SRV_CORES not set"; exit 1; fi
	if [ -z "$(SRV_CONC)" ]; then echo "Error: SRV_CONC not set"; exit 1; fi
	if [ -z "$(RECV_CORES)" ]; then echo "Error: RECV_CORES not set"; exit 1; fi
	if [ -z "$(RECV_CONC)" ]; then echo "Error: RECV_CONC not set"; exit 1; fi
	if [ -z "$(TPUT)" ]; then echo "Error: TPUT not set"; exit 1; fi
	if [ -z "$(DUR)" ]; then echo "Error: DUR not set"; exit 1; fi
	for SIZE in $(SIZES); do \
		$(MAKE) -f $(MAKEFILE_LIST) $(LOCAL_RESULTS)/rtt_$(TAG)_s$${SIZE}_send$(SEND_CORES)c$(SEND_CONC)_srv$(SRV_CORES)c$(SRV_CONC)_recv$(RECV_CORES)c$(RECV_CONC)_d$(DUR)_l$(TPUT).csv; \
	done

throughput-vs-size:
	@if [ -z "$(SIZES)" ]; then echo "Error: SIZES not set"; exit 1; fi
	if [ "$(MODE)" != "sync" ] && [ "$(MODE)" != "async" ]; then echo "Error: MODE must be sync or async (got '$(MODE)')"; exit 1; fi
	if [ -z "$(PROD_CORES)" ]; then echo "Error: PROD_CORES not set"; exit 1; fi
	if [ -z "$(PROD_CONC)" ]; then echo "Error: PROD_CONC not set"; exit 1; fi
	if [ -z "$(CONS_CORES)" ]; then echo "Error: CONS_CORES not set"; exit 1; fi
	if [ -z "$(CONS_CONC)" ]; then echo "Error: CONS_CONC not set"; exit 1; fi
	if [ -z "$(BATCHES)" ]; then echo "Error: BATCHES not set"; exit 1; fi
	if [ -z "$(BSIZE)" ]; then echo "Error: BSIZE not set"; exit 1; fi
	if [ -z "$(DUR)" ]; then echo "Error: DUR not set"; exit 1; fi
	for SIZE in $(SIZES); do \
		$(MAKE) -f $(MAKEFILE_LIST) $(LOCAL_RESULTS)/throughput_$(TAG)_s$${SIZE}_prod$(PROD_CORES)c$(PROD_CONC)_cons$(CONS_CORES)c$(CONS_CONC)_b$(BATCHES)_bs$(BSIZE)_d$(DUR).csv; \
	done

help:
	@echo "C++ / libvalkey benchmark harness."
	@echo ""
	@echo "Variables:"
	@echo "  HOST1        = $(HOST1)       (sender + receiver host)"
	@echo "  HOST2        = $(HOST2)       (Valkey broker host)"
	@echo "  HOST3        = $(HOST3)       (echo server host)"
	@echo "  VALKEY_URL   = $(VALKEY_URL)"
	@echo "               use redis://host:port for TCP or rdma://host:port for RDMA"
	@echo "  REMOTE_VALKEY = $(REMOTE_VALKEY)  (remote checkout path)"
	@echo "  TRANSPORT    = $(TRANSPORT)       (list)"
	@echo "  MODE         = $(MODE)       (sync | async; optional, applies to rtt send/server and throughput produce)"
	@echo ""
	@echo "Sweep targets:"
	@echo "  make latency-vs-size       [MODE=sync|async] SIZES=... SEND_CORES=... SEND_CONC=... SRV_CORES=... SRV_CONC=... RECV_CORES=... RECV_CONC=... TPUT=... DUR=..."
	@echo "  make throughput-vs-size    [MODE=sync|async] SIZES=... PROD_CORES=... PROD_CONC=... CONS_CORES=... CONS_CONC=... BATCHES=... BSIZE=... DUR=..."
	@echo ""
	@echo "Result tags: MODE=sync keeps the bare transport (rtt_list_..., throughput_list_..., the"
	@echo "published filenames); MODE=async appends -async (rtt_list-async_..., throughput_list-async_...)."
