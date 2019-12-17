
#!/bin/bash

# dma channel stats
echo "dma channels stats (bytes per channel)"
cat /sys/class/dma/dma*chan*/bytes_transferred | nl -w2 -s':  ' | awk '$0="dma channel"$0'

echo "======================================================================" 
echo "dma channels stats (memcpy ops per channel)"
cat /sys/class/dma/dma*chan*/memcpy_count | nl -w2 -s':  '| awk '$0="dma channel"$0'
echo "======================================================================"

export CPU_CYC_PER_USEC=$(dmesg | grep tsc | grep ca | grep -Eo '[+-]?[0-9]+([.][0-9]+)?' | sed -n '2p')

if [ !$CPU_CYC_PER_USEC ]; then
	echo "NOTE: dmesg was probably wrapped around, setting to default freq $CPU_CYC_PER_USEC"
	export CPU_CYC_PER_USEC=2200
fi

echo "cpu freq is: $CPU_CYC_PER_USEC"

USEC_IN_SEC=1000^2

SHIFT_SAVE_BITS=1024

for (( ; ; ))
do 
        a=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep post_send_bytes | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')

	poll=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep poll_cq_count | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')

	cycles1=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep accum_latency | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')

        cqes1=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep cqes_polled | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}') 

	sleep 1; 

	b=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep post_send_bytes | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')

	poll_next=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep poll_cq_count | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')

        cycles2=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep accum_latency | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')
 
        cqes2=$(cat /sys/kernel/debug/ntrdma/ntrdma_0/perf |  grep cqes_polled | grep -o -E '[0-9]+' | awk '{s+=$1} END {printf "%.0f\n", s}')

	c=`expr $b - $a`       
        if [ ${c} -ne 0 ] 
		then 
		bw=$(awk "BEGIN {printf \"%.3f\",${c}/1024^3}")
		echo "bw: ${bw} GB/s ${c} bytes/sec" 
	fi
        
        cycles=`expr $cycles2 - $cycles1`
        cqes=`expr $cqes2 - $cqes1`

        if [[ ${cqes} -ne 0 && ${cycles} -ne 0 ]]; then lat=$(awk "BEGIN {printf \"%.3f\",${cycles}*${SHIFT_SAVE_BITS}/($cqes*$CPU_CYC_PER_USEC)}"); echo "latency: $lat usec"; fi
	
	diff=`expr $poll_next - $poll`
	if [[ ${diff} -ne 0 && ${cqes} -ne 0 ]] 
		then 
		poll_cyc=$(awk 'BEGIN {printf "%.3f\n", '$USEC_IN_SEC/$diff' }'); echo "user polling : $poll_cyc usec"
	fi


done

