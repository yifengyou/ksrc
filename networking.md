# networking

```
	#define local_softirq_pending() (__this_cpu_read(local_softirq_pending_ref))
	#define local_softirq_pending() (__this_cpu_read(local_softirq_pending_ref))
		alskdjf
static int ksoftirqd_should_run(unsigned int cpu)
	#define local_softirq_pending() (__this_cpu_read(local_softirq_pending_ref))
	alskdf
		alskdjf
		laksjdf
	alskdjf
	alksjdf
		alksjdf
	laksjd
		alksjdf
	laskdjf
off

* kj_soft

static int ksoftirqd_should_run(unsigned int cpu)

networking

* static void run_ksoftirqd(unsigned int cpu)
	* local_irq_disable();
* __do_softirq();
            * h = softirq_vec;
            * h->action(h);
                * net_rx_action(h)
                    * igb_poll(napi,budget)
                        * igb_clean_rx_irq(q_vector, budget);
                            * rx_buffer = igb_get_rx_buffer(rx_ring, size);
                            * igb_put_rx_buffer(rx_ring, rx_buffer);
                            * igb_is_non_eop(rx_ring, rx_desc)
                            * igb_cleanup_headers(rx_ring, rx_desc, skb)
                            * igb_process_skb_fields(rx_ring, rx_desc, skb);
                            * napi_gro_receive(&q_vector->napi, skb);
                        * napi_complete_done(napi, work_done);
    * local_irq_enable();
```


---
