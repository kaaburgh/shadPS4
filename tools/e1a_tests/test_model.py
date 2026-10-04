import unittest
from model import FakeScheduler, SubmittedTick

class PrefixContractScaffolding(unittest.TestCase):
    def test_store_and_irq_wait_for_completion(self):
        s=FakeScheduler();s.record('draw A');t=s.guest_fence(7)
        s.progress();self.assertEqual(s.events,[])
        s.complete(t);s.worker_step();self.assertEqual(s.events,[])
        s.cp_step();self.assertEqual(s.events,[('store',7,t.value),('irq',7,t.value)])

    def test_post_fence_work_is_excluded_and_does_not_delay_signal(self):
        s=FakeScheduler();s.record('A');a=s.guest_fence(1)
        s.record('B');b=s.flush_prefix()
        self.assertEqual(s.submitted[a.value],('A',));self.assertEqual(s.submitted[b.value],('B',))
        s.complete(a);s.progress();self.assertEqual(s.state.label,1)
        self.assertLess(s.completed_tick,b.value)

    def test_idle_cp_requires_no_later_submit_or_scheduler_pop(self):
        s=FakeScheduler();s.record('A');t=s.guest_fence(9)
        self.assertEqual(s.submit_count,1)
        s.complete(t);s.progress();self.assertEqual(s.state.label,9)
        self.assertEqual(s.submit_count,1)

    def test_multiple_fences_are_fifo_and_store_precedes_own_irq(self):
        s=FakeScheduler();s.record('A');a=s.guest_fence(1);s.record('B');b=s.guest_fence(2)
        s.complete(b);s.progress()
        self.assertEqual(s.events,[('store',1,a.value),('irq',1,a.value),('store',2,b.value),('irq',2,b.value)])

    def test_existing_finish_proof_is_not_submitted_or_waited_again(self):
        s=FakeScheduler();s.record('A');t=s.existing_finish()
        s.guest_fence(3,completed_prefix=t);s.progress()
        self.assertEqual(s.submit_count,1);self.assertEqual(s.state.label,3)

    def test_completed_sync_fence_cannot_overtake_older_pending_fence(self):
        s=FakeScheduler();s.record('A');a=s.guest_fence(1)
        s.record('B');b=s.existing_finish();s.guest_fence(2,completed_prefix=b)
        s.progress()
        self.assertEqual(s.submit_count,2)
        self.assertEqual(s.events,[('store',1,a.value),('irq',1,a.value),('store',2,b.value),('irq',2,b.value)])

    def test_wrong_or_prospective_ticket_is_rejected(self):
        s=FakeScheduler()
        with self.assertRaises(ValueError):s.guest_fence(1,completed_prefix=SubmittedTick('draw',1))
        t=s.flush_prefix()
        with self.assertRaises(ValueError):s.guest_fence(1,completed_prefix=SubmittedTick('present',t.value))

    def test_submit_failure_cannot_publish_success(self):
        s=FakeScheduler();s.record('A')
        with self.assertRaises(RuntimeError):s.flush_prefix(fail=True)
        s.progress();self.assertEqual(s.events,[])

    def test_device_lost_does_not_manufacture_completion(self):
        s=FakeScheduler();s.record('A');s.guest_fence(1);s.device_lost();s.progress()
        self.assertEqual(s.completed_tick,0);self.assertEqual(s.events,[])

    def test_shutdown_cancels_pending_gpu_wait_and_ready_cp_action(self):
        for gpu_ready in (False,True):
            with self.subTest(gpu_ready=gpu_ready):
                s=FakeScheduler();s.record('A');t=s.guest_fence(1)
                if gpu_ready:s.complete(t);s.worker_step()
                s.shutdown();s.progress();self.assertEqual(s.events,[])

    def test_packet_payload_is_owned_and_not_a_borrowed_cmd_pointer(self):
        packet={'data':123};s=FakeScheduler();t=s.guest_fence(packet['data'])
        packet['data']=456;s.complete(t);s.progress();self.assertEqual(s.state.label,123)

    def test_memory_compare_can_progress_from_pending_fence(self):
        s=FakeScheduler();s.record('A');t=s.guest_fence(8)
        self.assertNotEqual(s.state.label,8)
        s.complete(t);s.progress();self.assertEqual(s.state.label,8)

    def test_blocking_vo_optimization_is_a_real_integration_obligation(self):
        s=FakeScheduler();s.record('A');t=s.guest_fence(5)
        s.cp_blocking_wait=True;s.complete(t);s.progress();self.assertEqual(s.events,[])
        s.cp_blocking_wait=False;s.cp_step();self.assertEqual(s.state.label,5)

    def test_gds_requires_a_prefix_snapshot_not_live_buffer_read(self):
        gds=[11];s=FakeScheduler();s.record(('snapshot GDS',gds[0]));t=s.guest_fence(gds[0])
        gds[0]=22  # Models a later GPU write before the CP publishes the old result.
        s.complete(t);s.progress();self.assertEqual(s.state.label,11)

if __name__=='__main__':unittest.main()
