"""Test-only deterministic design model. Never imported by emulator production code.

This is executable test design, not proof that Vulkan/Liverpool implements E1.
"""
from collections import deque
from dataclasses import dataclass

@dataclass(frozen=True)
class SubmittedTick:
    scheduler: str
    value: int

@dataclass
class GuestState:
    alive: bool = True
    label: int = 0

@dataclass(frozen=True)
class OwnedSignal:
    value: int
    irq: bool = True

class FakeScheduler:
    def __init__(self, name='draw'):
        self.name=name
        self.current_tick=1
        self.completed_tick=0
        self.recording=[]
        self.submitted={}
        self.submit_count=0
        self.cp_blocking_wait=False
        self.jobs=deque()
        self.cp_ready=deque()
        self.state=GuestState()
        self.events=[]
        self.failed=False

    def record(self, work):
        self.recording.append(work)

    def flush_prefix(self, fail=False):
        # No GPU completion wait. Captures the exact currently recorded prefix.
        if fail:
            self.device_lost()
            raise RuntimeError('submit failed; no valid submitted ticket')
        tick=self.current_tick
        self.submitted[tick]=tuple(self.recording)
        self.recording.clear()
        self.current_tick+=1
        self.submit_count+=1
        return SubmittedTick(self.name,tick)

    def existing_finish(self):
        ticket=self.flush_prefix()
        self.complete(ticket)
        return ticket

    def guest_fence(self, value, irq=True, completed_prefix=None):
        # completed_prefix is an explicit proof from an existing synchronous path.
        ticket=completed_prefix or self.flush_prefix()
        if ticket.scheduler!=self.name or ticket.value not in self.submitted:
            raise ValueError('only this scheduler successful submitted ticks may be waited')
        signal=OwnedSignal(value,irq)
        self.jobs.append((ticket,signal))
        return ticket

    def complete(self, ticket):
        if ticket.scheduler!=self.name or ticket.value not in self.submitted:
            raise ValueError('unsubmitted/wrong-scheduler completion')
        self.completed_tick=max(self.completed_tick,ticket.value)

    def worker_step(self):
        # Dedicated known-submitted guest lane; resource jobs waiting on future ticks
        # cannot block it. No renderer calls, stores or IRQs on this worker.
        if self.failed or not self.state.alive:
            self.jobs.clear()
            return
        while self.jobs and self.jobs[0][0].value<=self.completed_tick:
            ticket,signal=self.jobs.popleft()
            self.cp_ready.append((ticket,signal))

    def cp_step(self):
        # Production integration must make VO waits cooperative/message-aware.
        if self.cp_blocking_wait:
            return
        while self.cp_ready:
            ticket,signal=self.cp_ready.popleft()
            if self.failed or not self.state.alive:
                continue
            self.state.label=signal.value
            self.events.append(('store',signal.value,ticket.value))
            if signal.irq:
                self.events.append(('irq',self.state.label,ticket.value))

    def progress(self):
        self.worker_step()
        self.cp_step()

    def shutdown(self):
        # Models cancellation/join contract, not actual jthread/Vulkan behavior.
        self.state.alive=False
        self.jobs.clear()
        self.cp_ready.clear()

    def device_lost(self):
        self.failed=True
        self.jobs.clear()
        self.cp_ready.clear()
