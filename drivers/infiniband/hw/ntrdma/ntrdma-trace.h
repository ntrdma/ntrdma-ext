#undef TRACE_SYSTEM
#define TRACE_SYSTEM ntrdma

#if !defined(_NTRDMA_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NTRDMA_TRACE_H

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(imm_template,
	TP_PROTO(u64 wrid, u64 data, u64 dva, u64 dpa, u64 len, u32 rc),
	TP_ARGS(wrid, data, dva, dpa, len, rc),
	TP_STRUCT__entry(
			__field(u64, wrid)
			__field(u64, data)
			__field(u64, dva)
			__field(u64, dpa)
			__field(u64, len)
			__field(u32, rc)
	),
	TP_fast_assign(
			__entry->wrid = wrid;
			__entry->data = data;
			__entry->dva = dva;
			__entry->dpa = dpa;
			__entry->len = len;
			__entry->rc = rc;
			),
	TP_printk(
			"wrid 0x%llx snd data 0x%llx, dst phy 0x%llx dst vir 0x%llx, len 0x%llx, rc %d",
			__entry->wrid, __entry->data, __entry->dpa,
			__entry->dva, __entry->len, __entry->rc)
);

/*trace_imm_dma_cpy*/
DEFINE_EVENT(imm_template, imm_dma_cpy,
	TP_PROTO(u64 wrid, u64 data, u64 dva, u64 dpa, u64 len, u32 rc),
	TP_ARGS(wrid, data, dva, dpa, len, rc));

DECLARE_EVENT_CLASS(dma_template,
	TP_PROTO(u64 wrid, u64 sva, u64 spa, u64 dva,
			u64 dpa, u64 len, u32 rc),
	TP_ARGS(wrid, sva, spa, dva, dpa, len, rc),
	TP_STRUCT__entry(
			__field(u64, wrid)
			__field(u64, sva)
			__field(u64, spa)
			__field(u64, dva)
			__field(u64, dpa)
			__field(u64, len)
			__field(u32, rc)
	),
	TP_fast_assign(
			__entry->wrid = wrid;
			__entry->sva = sva;
			__entry->spa = spa;
			__entry->dva = dva;
			__entry->dpa = dpa;
			__entry->len = len;
			__entry->rc = rc;
			),
	TP_printk(
		"wrid 0x%llx src phy 0x%llx src vir 0x%llx, dst phy 0x%llx dst vir 0x%llx, len 0x%llx, rc %d",
		__entry->wrid, __entry->spa, __entry->sva, __entry->dpa,
		__entry->dva, __entry->len, __entry->rc)
);

/*trace_dma_cpy*/
DEFINE_EVENT(dma_template, dma_cpy,
		TP_PROTO(u64 wrid, u64 sva, u64 spa, u64 dva,
				u64 dpa, u64 len, u32 rc),
		TP_ARGS(wrid, sva, spa, dva, dpa, len, rc));

DECLARE_EVENT_CLASS(poll_ioctl_template,
	TP_PROTO(u64 wrid, u32 wc_opcode, u32 cq_opcode, s32 qp,
			s32 status, u32 pos, u32 end, u32 flags),
	TP_ARGS(wrid, wc_opcode, cq_opcode, qp, status, pos, end, flags),
	TP_STRUCT__entry(
			__field(u64, wrid)
			__field(u32, wc_opcode)
			__field(u32, cq_opcode)
			__field(u32, qp)
			__field(u32, status)
			__field(u32, pos)
			__field(u32, end)
			__field(u32, flags)
	),
	TP_fast_assign(
			__entry->wrid = wrid;
			__entry->wc_opcode = wc_opcode;
			__entry->cq_opcode = cq_opcode;
			__entry->qp = qp;
			__entry->status = status;
			__entry->pos = pos;
			__entry->end = end;
			__entry->flags = flags;
			),
	TP_printk(
			"OPCODE %d(%d): wrid %llu QP %d status %d pos %u end %u flags %d\n",
			__entry->wc_opcode,
			__entry->cq_opcode,
			__entry->wrid,
			__entry->qp,
			__entry->status,
			__entry->pos,
			__entry->end,
			__entry->flags)
);

/*trace_poll_ioctl*/
DEFINE_EVENT(poll_ioctl_template, poll_ioctl,
	TP_PROTO(u64 wrid, u32 wc_opcode, u32 cq_opcode, s32 qp,
			s32 status, u32 pos, u32 end, u32 flags),
	TP_ARGS(wrid, wc_opcode, cq_opcode, qp, status, pos, end, flags));
/*trace_poll_cq*/
DEFINE_EVENT(poll_ioctl_template, poll_cq,
	TP_PROTO(u64 wrid, u32 wc_opcode, u32 cq_opcode, s32 qp,
			s32 status, u32 pos, u32 end, u32 flags),
	TP_ARGS(wrid, wc_opcode, cq_opcode, qp, status, pos, end, flags));

DECLARE_EVENT_CLASS(rqp_work_template,
	TP_PROTO(u32 opcode, u32 flags, u64 addr, s32 qp,
			s32 num_segs, u64 wrid, s32 status),
	TP_ARGS(opcode, flags, addr, qp, num_segs, wrid, status),
	TP_STRUCT__entry(
			__field(u32, opcode)
			__field(u32, flags)
			__field(u64, addr)
			__field(s32, qp)
			__field(u32, num_segs)
			__field(u64, wrid)
			__field(s32, status)
	),
	TP_fast_assign(
			__entry->opcode = opcode;
			__entry->flags = flags;
			__entry->addr = addr;
			__entry->qp = qp;
			__entry->num_segs = num_segs;
			__entry->wrid = wrid;
			__entry->status = status;
			),
	TP_printk(
			"OPCODE %d: flags %x, addr %llx QP %d num sges %d wrid %llu status %d",
			__entry->opcode, __entry->flags, __entry->addr,
			__entry->qp, __entry->num_segs, __entry->wrid,
			__entry->status)
);

/*trace_rqp_work*/
DEFINE_EVENT(rqp_work_template, rqp_work,
		TP_PROTO(u32 opcode, u32 flags, u64 addr, s32 qp,
				s32 num_segs, u64 wrid, s32 status),
		TP_ARGS(opcode, flags, addr, qp, num_segs, wrid, status));

#endif /* _NTRDMA_TRACE_H */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE ntrdma-trace
#include <trace/define_trace.h>
