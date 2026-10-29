namespace rec=fh1::recorded;
std::atomic<uint32_t> record_ids{0},record_lines{0},record_events{0};
thread_local rec::Producer record_producer;
thread_local uint32_t record_revision=0;thread_local std::string record_identity;thread_local record_owner::Witness native_record_proof;
thread_local bool record_attempt=false;
bool RecordEnabled(){return REXCVAR_GET(forza_pause_recorded_provenance)&&REXCVAR_GET(forza_pause_provenance);}
bool RecordLine(){if(!RecordEnabled()||!REXCVAR_GET(forza_pause_provenance_diagnostics))return false;auto n=record_lines.load();while(n<512){if(record_lines.compare_exchange_weak(n,n+1))return true;}return false;}
rec::Retention RecordRetention(const RecordState& s,uint32_t device){return {device,s.v[4],s.v[5],s.v[7]};}
bool RecordEventLine(){if(!RecordEnabled()||!REXCVAR_GET(forza_pause_provenance_diagnostics))return false;auto n=record_events.load();while(n<128){if(record_events.compare_exchange_weak(n,n+1))return true;}return false;}
bool WriterDiagnostics(){return REXCVAR_GET(forza_pause_persistent_partial)&&RecordEnabled()&&REXCVAR_GET(forza_pause_provenance_diagnostics);}
void WriterReject(uint32_t reason,uint32_t address,uint32_t width){if(WriterDiagnostics())writer_diagnostic.Note(reason,address,width);}
template<class Values> std::string WriterValues(const Values& values){std::string out;for(auto v:values){if(!out.empty())out+=",";out+=std::to_string(v);}return out;}
void WriterSummary(const char* outcome,uint32_t sealed){
  if(!WriterDiagnostics()||!writer_diagnostic.uid||writer_diagnostic.emitted||(frame.token&&std::strcmp(outcome,"replaced-before-finalize")!=0))return;
  writer_diagnostic.emitted=true;auto n=writer_summary_lines.load();while(n<512&&!writer_summary_lines.compare_exchange_weak(n,n+1)){}if(n>=512)return;
  const auto& d=writer_diagnostic;const auto& a=d.events?d.first_attempt:d.current;const auto& e=record_producer.retention;
  REXLOG_INFO("FH1WF record us={} uid={} outcome={} events={} first={} last={} counts={} op={} serial={} phase={} host_depth={} returned={:08X} address={:08X} width={} native_proof={} expected_device={:08X} expected_anchor={:08X} expected_generation={} expected_ring={} before_taken={} before_readable={} before_mask={} before={} after_taken={} after_readable={} after_mask={} after={} closed={} failed={} submitted={} producer_reason={} seal_reason={} sealed={}",DiagnosticTime(),d.uid,outcome,d.events,d.first,d.last,WriterValues(d.counts),a.op,a.serial,a.phase,a.depth,a.returned,a.address,a.width,a.native_proof,e.device,e.anchor,e.generation,e.ring,a.before_taken,a.before_readable,a.before_mask,WriterValues(a.before),a.after_taken,a.after_readable,a.after_mask,WriterValues(a.after),record_producer.closed,record_producer.failed,record_producer.submitted,uint32_t(record_producer.failure),uint32_t(record_producer.seal_failure),sealed);
}
void RecordRevoke(const char* reason,uint32_t next_uid=0){const auto old=REXCVAR_GET(forza_pause_recorded_sealed_uid);rec::Revoke(old,[](uint32_t){rex::cvar::SetFlagByName("forza_pause_recorded_sealed_uid","0");});if((old||next_uid)&&RecordEventLine())REXLOG_INFO("FH1RL revoke us={} reason={} old_uid={} next_uid={} published={} revision={}",DiagnosticTime(),reason,old,next_uid,REXCVAR_GET(forza_pause_recorded_sealed_uid),revision.load());}
void RecordStart(){
  WriterSummary("replaced-before-finalize");record_attempt=false;if(!RecordEnabled())return;
  const auto s=RecordSnapshot(frame.base,frame.device);if(!s.readable||!s.v[4])return;
  const auto uid=p::NextId(record_ids);RecordRevoke("record-start",uid);record_attempt=true;record_revision=partial_identity_revision;record_identity=partial_record_identity;
  record_producer.Start(uid,RecordRetention(s,frame.device));native_record_proof.Start(frame.device,record_producer.retention,[&](uint32_t at,uint32_t& value){return Word(frame.base,at,value);});if(WriterDiagnostics())writer_diagnostic.Start(uid);
}
struct RecordedWriter {
  Frame& f;guest_write::Transaction transaction;std::unique_lock<std::mutex> lifecycle_lock;uint32_t attempt_uid=record_producer.uid,returned=0,before_mask=0,after_mask=0;rec::AllocatorState before{},after{};
  explicit RecordedWriter(Frame& value):f(value){}
  bool Owner(const rec::AllocatorState& state,bool final){
    const bool ok=native_record_proof.Allow(f.device,state,final,[&](uint32_t at,uint32_t& value){return Word(f.base,at,value);});
    if(!ok&&WriterDiagnostics())writer_diagnostic.current.native_proof=uint32_t(native_record_proof.failure);
    return ok;
  }
  bool Match(rec::Retention expected,rec::Retention current){return expected==record_producer.retention&&current==rec::Retained(after,f.device)&&Owner(after,false);}
  rec::AllocatorState Read(uint32_t& mask){return ReadDeviceFields(f.base,f.device,&mask);}
  uint32_t Reserve(uint32_t words){
    before=Read(before_mask);
    if(WriterDiagnostics()){auto& a=writer_diagnostic.current;a.before_taken=true;a.before_readable=before.readable;a.before_mask=before_mask;a.before=before.v;}
    if(!before.readable){WriterReject(11);return 0;}
    if(!Owner(before,false)){WriterReject(12);return 0;}
    if(before.v[2]&0x80200000u){WriterReject(13);return 0;}
    // Only the native fast reserve path is qualified by this integration.
    if(!before.v[0]||(before.v[0]&3)||uint64_t(before.v[0])+rec::kBytes>before.v[1]){WriterReject(62);return 0;}
    // Copy the complete ABI context. Native allocator guest-memory sideeffects
    // are retained, but its register clobbers cannot escape to the hooked caller.
    returned=rec::ReserveCopied(*f.context,f.device,words,[&](PPCContext& copy){__imp__sub_8243D1A8(copy,f.base);});
    after=Read(after_mask);
    if(WriterDiagnostics()){auto& a=writer_diagnostic.current;a.returned=returned;a.after_taken=true;a.after_readable=after.readable;a.after_mask=after_mask;a.after=after.v;}
    if(RecordLine())REXLOG_INFO("FH1RW reserve us={} uid={} requested={} returned={:08X} before_cursor={:08X} after_cursor={:08X} hard={:08X} before_slab={:08X} after_slab={:08X} flags={:08X} anchor={:08X} anchor_gen={} ring_gen={} copied_context=true",
      DiagnosticTime(),record_producer.uid,words,returned,before.v[0],after.v[0],after.v[1],before.v[6],after.v[6],after.v[2],after.v[3],after.v[4],after.v[5]);
    return returned;
  }
  bool Snapshot(uint32_t& cursor,uint32_t& hard,uint32_t& flags,rec::Retention& r){cursor=after.v[0];hard=after.v[1];flags=after.v[2];r=rec::Retained(after,f.device);return before.readable&&after.readable;}
  bool Writable(uint32_t address,uint32_t bytes){
    auto* kernel=rex::system::kernel_state();auto* memory=kernel?kernel->memory():nullptr;
    if(uint64_t(f.device)+52>0xF0000000ull||!transaction.Begin(memory,f.base,address,bytes,f.device+48)){WriterReject(14,address,bytes);return false;}
    // SDK global lock serializes watch rearming; lifecycle lock serializes Cancel.
    lifecycle_lock=std::unique_lock<std::mutex>(eligibility_mutex);
    uint32_t mask=0;const auto now=Read(mask);
    if(!RecordEnabled()||record_revision!=revision.load()||record_producer.uid!=attempt_uid||record_producer.failed||record_producer.submitted||
       !now.readable||now.v!=after.v||!Owner(now,false)){
      lifecycle_lock.unlock();transaction.End();WriterReject(63);return false;
    }
    return true;
  }
  void Store(uint32_t address,uint32_t value){p::StoreBigEndian(GuestPtr(f.base,address),value);}
  void Commit(uint32_t cursor){Store(f.device+48,cursor);native_record_proof.Commit(f.device,after,returned+4,rec::kBytes,[&](uint32_t at,uint32_t& value){return Word(f.base,at,value);},[](uint32_t at){return DiagnosticPhysical(at);});lifecycle_lock.unlock();transaction.End();}
};
bool RecordSend(p::Op op,uint32_t serial,p::Phase phase,std::array<uint32_t,5> data){
  if(op==p::Op::FrameBegin)data={frame.scene.graphics,frame.scene.core,frame.scene.camera,frame.scene.car,uint32_t(frame.scene.generation)};
  const p::Record r{op,record_producer.uid,serial,phase,data};
  if(WriterDiagnostics())writer_diagnostic.Begin(uint32_t(op),serial,uint32_t(phase),uint32_t(host_depth));
  RecordedWriter io{frame};const bool valid=record_producer.Observe(r);
  if(!valid)WriterReject(17+uint32_t(record_producer.failure));
  const bool ok=valid&&rec::EmitValidated(io,RecordEnabled(),r,record_producer.retention,[](rec::EmitFailure why){WriterReject(40+uint32_t(why));},[&](rec::Retention expected,rec::Retention actual){return io.Match(expected,actual);});
  if(!ok)record_producer.Fail();
  if(RecordLine())REXLOG_INFO("FH1RW write us={} uid={} op={} serial={} phase={} header_phys={:08X} next={:08X} success={} closed={} failed={}",DiagnosticTime(),r.token,uint32_t(op),serial,uint32_t(phase),DiagnosticPhysical(io.returned+4),ok?io.returned+44:io.returned,ok,record_producer.closed,record_producer.failed);
  return ok;
}
void RecordFinalize(PPCContext& c,uint8_t* base){
  if(!RecordEnabled()||!record_producer.uid||record_producer.retention.device!=c.r3.u32)return;
  const auto s=RecordSnapshot(base,c.r3.u32);uint32_t sealed=0;bool published=false;
  {std::lock_guard lock(eligibility_mutex);
    if(s.readable&&record_revision==revision.load()){
      rec::AllocatorState allocator;allocator.v={s.v[0],s.v[1],s.v[3],s.v[4],s.v[5],s.v[7],s.v[6]};
      const bool owned=native_record_proof.Allow(c.r3.u32,allocator,true,[&](uint32_t at,uint32_t& value){return Word(base,at,value);});
      if(owned)sealed=record_producer.SealChecked(RecordRetention(s,c.r3.u32),frame.token!=0,s.v[3],[&](rec::Retention expected,rec::Retention actual){return expected==record_producer.retention&&actual==rec::Retained(allocator,c.r3.u32);});
      else {if(WriterDiagnostics())writer_diagnostic.current.native_proof=uint32_t(native_record_proof.failure);WriterReject(62);CancelLocked("native-owner-proof");}
    }
    else WriterReject(!s.readable?60:61);
    const auto admission=record_admission::Certificate(sealed,record_revision,revision.load(),record_identity,partial_record_identity);
    if(!admission.empty())rex::cvar::SetFlagByName("forza_pause_partial_admission",admission);
    published=rex::cvar::SetFlagByName("forza_pause_recorded_sealed_uid",std::to_string(sealed));}
  if(REXCVAR_GET(forza_pause_persistent_partial)&&REXCVAR_GET(forza_pause_provenance_diagnostics))rex::cvar::SetFlagByName("forza_pause_partial_seal",std::to_string(record_producer.uid)+","+std::to_string(sealed)+","+std::to_string(DiagnosticTime())+","+partial_record_identity);
  if(!frame.token&&!sealed&&record_producer.seal_failure!=rec::SealFailure::None)WriterReject(52+uint32_t(record_producer.seal_failure));
  WriterSummary("finalize",sealed);
  if(RecordEventLine())REXLOG_INFO("FH1RL seal us={} uid={} requested={} published_ok={} published_uid={} producer_revision={} current_revision={} world_active={} closed={} failed={} flags={:08X}",DiagnosticTime(),record_producer.uid,sealed,published,REXCVAR_GET(forza_pause_recorded_sealed_uid),record_revision,revision.load(),frame.token!=0,record_producer.closed,record_producer.failed,s.v[3]);
  if(RecordLine())REXLOG_INFO("FH1RW seal us={} uid={} sealed={} world_active={} closed={} failed={} anchor={:08X} anchor_gen={} ring_gen={} before_original_submission=true",DiagnosticTime(),record_producer.uid,sealed,frame.token!=0,record_producer.closed,record_producer.failed,s.v[4],s.v[5],s.v[7]);
}
