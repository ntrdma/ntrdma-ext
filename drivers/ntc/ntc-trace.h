#undef TRACE_SYSTEM
#define TRACE_SYSTEM ntc

#if !defined(_NTC_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NTC_TRACE_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(dma_comp_template,
	TP_PROTO(u64 wrid, u64 addr, size_t len, u32 result, u32 residue),
	TP_ARGS(wrid, addr, len, result, residue),
	TP_STRUCT__entry(
			__field(u64, wrid)
			__field(u64, addr)
			__field(size_t, len)
			__field(u32, result)
			__field(u32, residue)
	),
	TP_fast_assign(
			__entry->wrid = wrid;
			__entry->addr = addr;
			__entry->len = len;
			__entry->result = result;
			__entry->residue = residue;
			),
	TP_printk(
			"Completion of wrid %#llx addr %#llx len %zu, result %d, residue %d",
			__entry->wrid, __entry->addr, __entry->len,
			__entry->result, __entry->residue)
);

/*trace_post_send*/
DEFINE_EVENT(dma_comp_template, dma_completion,
	TP_PROTO(u64 wrid, u64 addr, size_t len, u32 result, u32 residue),
	TP_ARGS(wrid, addr, len, result, residue));
#endif /* _NTC_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE ntc-trace
#include <trace/define_trace.h>
