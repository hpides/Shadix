SHELL := bash
.ONESHELL:

# Machines (placeholders) — override with your hosts, e.g.
# `make latency-vs-size HOST1=... HOST2=...`.
HOST1 := node1
HOST2 := node2

# SSH with ControlMaster for connection reuse
SSH_CONTROL_DIR := ./sockets
SSH_OPTS := -o ControlMaster=auto -o ControlPath=$(SSH_CONTROL_DIR)/%r@%h -o ControlPersist=600
SSH := ssh $(SSH_OPTS)

# Remote paths
REMOTE_SHADIX := ~/shadix
REMOTE_RESULTS := ~/results

# Local paths
LOCAL_RESULTS := ./results

# CXL configuration
CXL_DEVICE := /dev/dax0.0
RTT_RESPONSE_OFFSET := 262144

# RTT Benchmark Rule
# Execution order: receiver (HOST1) -> server (HOST2) -> sender (HOST1)
# Target format: rtt_s<size>_send<cores>c<conc>_srv<cores>c<conc>_recv<cores>c<conc>_d<dur>_l<load>.csv
$(LOCAL_RESULTS)/rtt_%.csv: | $(LOCAL_RESULTS)
	@TARGET=$$(basename $@)
	SIZE=$$(echo "$$TARGET" | sed -n 's/.*rtt_s\([0-9]*\)_.*/\1/p')
	SEND_CORES=$$(echo "$$TARGET" | sed -n 's/.*_send\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	SEND_CONC=$$(echo "$$TARGET" | sed -n 's/.*_send[0-9+-]*c\([0-9]*\)_.*/\1/p')
	SRV_CORES=$$(echo "$$TARGET" | sed -n 's/.*_srv\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	SRV_CONC=$$(echo "$$TARGET" | sed -n 's/.*_srv[0-9+-]*c\([0-9]*\)_.*/\1/p')
	RECV_CORES=$$(echo "$$TARGET" | sed -n 's/.*_recv\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	RECV_CONC=$$(echo "$$TARGET" | sed -n 's/.*_recv[0-9+-]*c\([0-9]*\)_.*/\1/p')
	DUR=$$(echo "$$TARGET" | sed -n 's/.*_d\([0-9a-z]*\)_l.*/\1/p')
	LOAD=$$(echo "$$TARGET" | sed -n 's/.*_l\([0-9.]*\)\.csv/\1/p')
	# Count instances from cores specs (number of semicolons + 1)
	SEND_COUNT=$$(echo "$$SEND_CORES" | tr -cd ';' | wc -c); SEND_COUNT=$$((SEND_COUNT + 1))
	SRV_COUNT=$$(echo "$$SRV_CORES" | tr -cd ';' | wc -c); SRV_COUNT=$$((SRV_COUNT + 1))
	RECV_COUNT=$$(echo "$$RECV_CORES" | tr -cd ';' | wc -c); RECV_COUNT=$$((RECV_COUNT + 1))
	# Create SSH control directory
	mkdir -p $(SSH_CONTROL_DIR)
	# Initialize on HOST1 (also establishes ControlMaster connection)
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/rtt.just init \
	    $$SIZE $(CXL_DEVICE) 0 $(CXL_DEVICE) $(RTT_RESPONSE_OFFSET) $$SEND_COUNT $$SRV_COUNT $$RECV_COUNT"
	# Establish ControlMaster connection to HOST2
	$(SSH) $(HOST2) "true"
	# Start receiver on HOST1 (background) - reuses ControlMaster
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/rtt.just receive \
	    '$$RECV_CORES' $$SIZE $(CXL_DEVICE) $(RTT_RESPONSE_OFFSET) $$RECV_CONC $$DUR $(REMOTE_RESULTS)" &
	RECV_PID=$$!
	# Start server on HOST2 (background) - reuses ControlMaster
	$(SSH) $(HOST2) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/rtt.just server \
	    '$$SRV_CORES' $$SIZE $(CXL_DEVICE) 0 $(CXL_DEVICE) $(RTT_RESPONSE_OFFSET) $$SRV_CONC $$DUR" &
	SERVER_PID=$$!
	# Brief wait for processes to start (connections already established)
	sleep 0.5
	# Start sender on HOST1 (foreground)
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/rtt.just send \
	    '$$SEND_CORES' $$SIZE $(CXL_DEVICE) 0 $$SEND_CONC $$DUR $$LOAD"
	wait $$RECV_PID || true
	wait $$SERVER_PID || true
	# Rename and copy result
	# Just outputs: rtt_s<size>_c<conc>_d<dur>_r0.csv
	$(SSH) $(HOST1) "cat $(REMOTE_RESULTS)/rtt_s$${SIZE}_c$${RECV_CONC}_d$${DUR}_r*.csv | sort -r | tail -n+$${RECV_COUNT} > \
	    $(REMOTE_RESULTS)/$$TARGET"
	rsync -zvhP -e "ssh $(SSH_OPTS)" $(HOST1):$(REMOTE_RESULTS)/$$TARGET $@

# Throughput Benchmark Rule
# Execution order: consumer (HOST2) -> producer (HOST1)
# Target format: throughput_s<size>_prod<cores>c<conc>_cons<cores>c<conc>_b<batches>_bs<bsize>_d<dur>.csv
$(LOCAL_RESULTS)/throughput_%.csv: | $(LOCAL_RESULTS)
	@TARGET=$$(basename $@)
	SIZE=$$(echo "$$TARGET" | sed -n 's/.*throughput_s\([0-9]*\)_.*/\1/p')
	PROD_CORES=$$(echo "$$TARGET" | sed -n 's/.*_prod\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	PROD_CONC=$$(echo "$$TARGET" | sed -n 's/.*_prod[0-9+-]*c\([0-9]*\)_.*/\1/p')
	CONS_CORES=$$(echo "$$TARGET" | sed -n 's/.*_cons\([0-9+-]*\)c[0-9]*.*/\1/p' | tr '+' ';')
	CONS_CONC=$$(echo "$$TARGET" | sed -n 's/.*_cons[0-9+-]*c\([0-9]*\)_.*/\1/p')
	BATCHES=$$(echo "$$TARGET" | sed -n 's/.*_b\([0-9]*\)_bs.*/\1/p')
	BSIZE=$$(echo "$$TARGET" | sed -n 's/.*_bs\([0-9]*\)_d.*/\1/p')
	DUR=$$(echo "$$TARGET" | sed -n 's/.*_d\([0-9a-z]*\)\.csv/\1/p')
	# Count instances from cores specs (number of semicolons + 1)
	PROD_COUNT=$$(echo "$$PROD_CORES" | tr -cd ';' | wc -c); PROD_COUNT=$$((PROD_COUNT + 1))
	CONS_COUNT=$$(echo "$$CONS_CORES" | tr -cd ';' | wc -c); CONS_COUNT=$$((CONS_COUNT + 1))
	# Create SSH control directory
	mkdir -p $(SSH_CONTROL_DIR)
	# Initialize on HOST1 (also establishes ControlMaster connection)
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/throughput.just init \
	    $$SIZE $(CXL_DEVICE) 0 $$PROD_COUNT $$CONS_COUNT"
	# Establish ControlMaster connection to HOST2
	$(SSH) $(HOST2) "true"
	# Start consumer on HOST2 (background) - reuses ControlMaster
	$(SSH) $(HOST2) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/throughput.just consume \
	    '$$CONS_CORES' $$SIZE $(CXL_DEVICE) 0 $$CONS_CONC $$DUR" &
	CONSUMER_PID=$$!
	# Brief wait for process to start (connection already established)
	sleep 0.5
	# Start producer on HOST1 (foreground)
	$(SSH) $(HOST1) "source ~/.profile && cd $(REMOTE_SHADIX) && just -f just/throughput.just produce \
	    '$$PROD_CORES' $$SIZE $(CXL_DEVICE) 0 $$PROD_CONC $$BATCHES $$BSIZE $(REMOTE_RESULTS) 1s"
	wait $$CONSUMER_PID || true
	# Rename and copy results for all producer instances
	# Just outputs: throughput_s<size>_b<batches>_bs<bsize>_id<N>.csv
	for id in $$(seq 0 $$((PROD_COUNT - 1))); do \
		BASE=$${TARGET%.csv}; \
		$(SSH) $(HOST1) "mv $(REMOTE_RESULTS)/throughput_s$${SIZE}_b$${BATCHES}_bs$${BSIZE}_id$${id}.csv \
		    $(REMOTE_RESULTS)/$${BASE}_id$${id}.csv"; \
		rsync -zvhP -e "ssh $(SSH_OPTS)" $(HOST1):$(REMOTE_RESULTS)/$${BASE}_id$${id}.csv $(LOCAL_RESULTS)/; \
	done
	touch $@

# Utility Targets
$(LOCAL_RESULTS):
	mkdir -p $@

.PHONY: help latency-vs-size throughput-vs-size

# Latency vs Size Benchmark
# Runs RTT benchmarks across multiple item sizes
# Usage: make latency-vs-size SIZES="64 128 256" SEND_CORES=25 SEND_CONC=1 SRV_CORES=25 SRV_CONC=1 RECV_CORES=26 RECV_CONC=1 TPUT=1.0 DUR=10s
latency-vs-size:
	@if [ -z "$(SIZES)" ]; then echo "Error: SIZES not set"; exit 1; fi
	if [ -z "$(SEND_CORES)" ]; then echo "Error: SEND_CORES not set"; exit 1; fi
	if [ -z "$(SEND_CONC)" ]; then echo "Error: SEND_CONC not set"; exit 1; fi
	if [ -z "$(SRV_CORES)" ]; then echo "Error: SRV_CORES not set"; exit 1; fi
	if [ -z "$(SRV_CONC)" ]; then echo "Error: SRV_CONC not set"; exit 1; fi
	if [ -z "$(RECV_CORES)" ]; then echo "Error: RECV_CORES not set"; exit 1; fi
	if [ -z "$(RECV_CONC)" ]; then echo "Error: RECV_CONC not set"; exit 1; fi
	if [ -z "$(TPUT)" ]; then echo "Error: TPUT not set"; exit 1; fi
	if [ -z "$(DUR)" ]; then echo "Error: DUR not set"; exit 1; fi
	for SIZE in $(SIZES); do \
		$(MAKE) -f $(MAKEFILE_LIST) $(LOCAL_RESULTS)/rtt_s$${SIZE}_send$(SEND_CORES)c$(SEND_CONC)_srv$(SRV_CORES)c$(SRV_CONC)_recv$(RECV_CORES)c$(RECV_CONC)_d$(DUR)_l$(TPUT).csv; \
	done

# Throughput vs Size Benchmark
# Runs throughput benchmarks across multiple item sizes
# Usage: make throughput-vs-size SIZES="64 128 256" PROD_CORES=25 PROD_CONC=1 CONS_CORES=25 CONS_CONC=1 BATCHES=1000 BSIZE=1000 DUR=60s
throughput-vs-size:
	@if [ -z "$(SIZES)" ]; then echo "Error: SIZES not set"; exit 1; fi
	if [ -z "$(PROD_CORES)" ]; then echo "Error: PROD_CORES not set"; exit 1; fi
	if [ -z "$(PROD_CONC)" ]; then echo "Error: PROD_CONC not set"; exit 1; fi
	if [ -z "$(CONS_CORES)" ]; then echo "Error: CONS_CORES not set"; exit 1; fi
	if [ -z "$(CONS_CONC)" ]; then echo "Error: CONS_CONC not set"; exit 1; fi
	if [ -z "$(BATCHES)" ]; then echo "Error: BATCHES not set"; exit 1; fi
	if [ -z "$(BSIZE)" ]; then echo "Error: BSIZE not set"; exit 1; fi
	if [ -z "$(DUR)" ]; then echo "Error: DUR not set"; exit 1; fi
	for SIZE in $(SIZES); do \
		$(MAKE) -f $(MAKEFILE_LIST) $(LOCAL_RESULTS)/throughput_s$${SIZE}_prod$(PROD_CORES)c$(PROD_CONC)_cons$(CONS_CORES)c$(CONS_CONC)_b$(BATCHES)_bs$(BSIZE)_d$(DUR).csv; \
	done

help:
	@echo "Usage:"
	@echo "  make results/rtt_s<size>_send<cores>c<conc>_srv<cores>c<conc>_recv<cores>c<conc>_d<dur>_l<load>.csv"
	@echo "  make results/throughput_s<size>_prod<cores>c<conc>_cons<cores>c<conc>_b<batches>_bs<bsize>_d<dur>.csv"
	@echo "  make latency-vs-size SIZES=\"...\" SEND_CORES=... SEND_CONC=... SRV_CORES=... SRV_CONC=... RECV_CORES=... RECV_CONC=... TPUT=... DUR=..."
	@echo "  make throughput-vs-size SIZES=\"...\" PROD_CORES=... PROD_CONC=... CONS_CORES=... CONS_CONC=... BATCHES=... BSIZE=... DUR=..."
	@echo ""
	@echo "Cores encoding: + = semicolon (for multiple instances)"
	@echo ""
	@echo "Examples:"
	@echo "  make results/rtt_s64_send25c1_srv25c1_recv26c1_d10s_l1.0.csv"
	@echo "  make results/rtt_s64_send25-27c4_srv28-30c4_recv31-33c4_d10s_l1.0.csv"
	@echo "  make results/throughput_s64_prod25c1_cons25c1_b1000_bs1000_d60s.csv"
	@echo "  make latency-vs-size SIZES=\"64 128 256\" SEND_CORES=25 SEND_CONC=1 SRV_CORES=25 SRV_CONC=1 RECV_CORES=26 RECV_CONC=1 TPUT=1.0 DUR=10s"
	@echo "  make throughput-vs-size SIZES=\"64 128 256\" PROD_CORES=25 PROD_CONC=1 CONS_CORES=25 CONS_CONC=1 BATCHES=1000 BSIZE=1000 DUR=60s"
	@echo ""
	@echo "Variables (current values):"
	@echo "  HOST1=$(HOST1) HOST2=$(HOST2)  (benchmark hosts; placeholders — override)"
	@echo "  CXL_DEVICE=$(CXL_DEVICE)  RTT_RESPONSE_OFFSET=$(RTT_RESPONSE_OFFSET)"
	@echo "  REMOTE_SHADIX=$(REMOTE_SHADIX)  REMOTE_RESULTS=$(REMOTE_RESULTS)"
