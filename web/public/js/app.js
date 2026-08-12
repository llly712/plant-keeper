/* 小花园 - 前端 */
const API = '';
let token = localStorage.getItem('token')||'';
let currentUser = null;
let charts = {};
let currentTab = 'all';
let timelapseTimer = null;
let timelapsePhotos = [];
let timelapseIdx = 0;
let historyHours = 24;
let dashboardLoading = false;
let lastDataAt = null;
let plantTargets = [];
let servoStateTimer = null;
let servoDragging = false;
let servoSendTimer = null;
let timelapsePlaying = false;

function toggleSidebar() {
  document.getElementById('sidebar').classList.toggle('open');
}

document.addEventListener('DOMContentLoaded', () => {
  if(token) fetchMe();
  initNav();
  updateAuthUI();
  loadDashboard();
  setInterval(loadDashboard, 30000);
  setInterval(updateClock, 1000);
  updateClock();
  document.querySelectorAll('.range-buttons button').forEach(btn=>btn.addEventListener('click',()=>{
    historyHours=parseInt(btn.dataset.hours)||24;
    document.querySelectorAll('.range-buttons button').forEach(x=>x.classList.toggle('active',x===btn));
    loadDashboard(true);
  }));
  const slider=document.getElementById('servoSlider');
  slider.addEventListener('pointerdown',()=>servoDragging=true);
  slider.addEventListener('input',()=>{
    document.getElementById('servoVal').textContent=slider.value;
    clearTimeout(servoSendTimer);
    if(token) servoSendTimer=setTimeout(()=>sendDeviceCommand('move',0,parseInt(slider.value),false),250);
  });
  slider.addEventListener('change',()=>{
    servoDragging=false;
    clearTimeout(servoSendTimer);
    if(token) sendDeviceCommand('move',0,parseInt(slider.value),false);
  });
  // 点击内容区关闭侧边栏
  document.getElementById('main').addEventListener('click', () => {
    document.getElementById('sidebar').classList.remove('open');
  });
  // 巡检列表事件委托
  const patrolBox=document.getElementById('patrolList');
  if(patrolBox) {
    let touchStartX=0,touchStartY=0;
    patrolBox.addEventListener('pointerup', onPatrolClick);
    patrolBox.addEventListener('click', onPatrolClick);
  }
});

function updateClock() {
  const el = document.getElementById('liveTime');
  if(!el) return;
  if(!lastDataAt) { el.textContent='● 正在连接'; el.className='live-badge'; return; }
  const sec=Math.floor((Date.now()-lastDataAt.getTime())/1000);
  if(sec<90) { el.textContent='● 在线 · '+sec+'秒前'; el.className='live-badge online'; }
  else if(sec<600) { el.textContent='● 延迟 · '+Math.floor(sec/60)+'分钟前'; el.className='live-badge stale'; }
  else { el.textContent='● 设备离线'; el.className='live-badge offline'; }
}

// ======================== 导航 ========================
function initNav() {
  document.querySelectorAll('.nav-item,.bn-item').forEach(item => {
    item.addEventListener('click', e => {
      e.preventDefault(); navigate(item.dataset.page);
      document.getElementById('sidebar').classList.remove('open');
    });
  });
  document.querySelectorAll('.tab').forEach(tab => {
    tab.addEventListener('click', () => { currentTab=tab.dataset.tab; document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active')); tab.classList.add('active'); loadStrategies(); });
  });
}
function navigate(page) {
  document.querySelectorAll('.nav-item,.bn-item').forEach(n=>n.classList.remove('active'));
  document.querySelectorAll(`.nav-item[data-page="${page}"],.bn-item[data-page="${page}"]`).forEach(n=>n.classList.add('active'));
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.getElementById(`page-${page}`)?.classList.add('active');
  if(page==='dashboard') loadDashboard();
  else if(page==='strategies') loadStrategies();
  else if(page==='device') { loadDeviceConfig(); startServoState(); }
  else if(page==='gallery') loadGallery();
  if(page!=='device') { clearInterval(servoStateTimer); servoStateTimer=null; }
  if(page!=='gallery') stopTimelapse(true);
}

// ======================== 认证 ========================
function showAuth(m) {
  document.getElementById('authModal').style.display='flex';
  if(m==='login') { document.getElementById('authTitle').textContent='登录'; document.getElementById('authBtn').textContent='登录'; document.getElementById('authSwitch').innerHTML='没有账号？去注册 (｀・ω・´)'; document.getElementById('authModal').dataset.mode='login'; }
  else { document.getElementById('authTitle').textContent='注册'; document.getElementById('authBtn').textContent='注册'; document.getElementById('authSwitch').innerHTML='已有账号？<u>去登录</u>'; document.getElementById('authModal').dataset.mode='register'; }
}
function toggleAuth() { showAuth(document.getElementById('authModal').dataset.mode==='login'?'register':'login'); }
function closeAuth() { document.getElementById('authModal').style.display='none'; }
async function doAuth() {
  const m=document.getElementById('authModal').dataset.mode;
  const u=document.getElementById('authUser').value.trim(), p=document.getElementById('authPass').value.trim();
  if(!u||!p) return toast('请填写完整');
  const r=await fetch(`${API}/api/auth/${m}`,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})});
  const d=await r.json();
  if(d.token) { token=d.token; currentUser=d.user; localStorage.setItem('token',token); closeAuth(); updateAuthUI(); toast('欢迎，'+d.user.username); if(document.querySelector('#page-strategies.active')) loadStrategies(); }
  else toast(d.error||'操作失败');
}
async function fetchMe() { try { const r=await fetch(`${API}/api/auth/me`,{headers:{'Authorization':`Bearer ${token}`}}); if(r.ok) currentUser=await r.json(); else { token=''; localStorage.removeItem('token'); } updateAuthUI(); } catch(e){} }
function logout() { token=''; currentUser=null; localStorage.removeItem('token'); updateAuthUI(); toast('已退出'); }
function updateAuthUI() {
  document.getElementById('btnLogin').style.display=currentUser?'none':'block';
  document.getElementById('btnLogout').style.display=currentUser?'block':'none';
  document.getElementById('userInfo').style.display=currentUser?'flex':'none';
  if(currentUser) document.getElementById('navUsername').textContent=currentUser.username;
}
function requireAuth() { if(!token) { toast('请先登录'); showAuth('login'); return false; } return true; }

async function api(url, opts={}) {
  const h={...opts.headers}; if(token) h['Authorization']=`Bearer ${token}`;
  if(opts.body&&typeof opts.body==='object'&&!(opts.body instanceof FormData)) { h['Content-Type']='application/json'; opts.body=JSON.stringify(opts.body); }
  return fetch(`${API}${url}`,{...opts,headers:h});
}

// ======================== 总览 ========================
async function loadDashboard(force=false) {
  if(dashboardLoading) return;
  if(!force && (!document.querySelector('#page-dashboard.active') || document.hidden)) return;
  dashboardLoading=true;
  try {
    const points=window.innerWidth<600?96:240;
    const [latest,history] = await Promise.all([api('/api/latest').then(checkJson),api(`/api/history?hours=${historyHours}&points=${points}`).then(checkJson)]);
    if(latest.temperature!==undefined) {
      lastDataAt=parseDbTime(latest.timestamp);
      document.getElementById('statTemp').textContent=latest.temperature?.toFixed(1)||'--';
      document.getElementById('statHum').textContent=latest.humidity?.toFixed(0)||'--';
      document.getElementById('statHealth').textContent=(latest.health||0)+'%';
      document.getElementById('statEC').textContent=latest.ec||'--';
      document.getElementById('statStage').textContent=latest.stage_name||'--';
      document.getElementById('statLight').textContent=latest.light||'--';
      updatePumps(latest);
    }
    if(latest.ai_class>0 && latest.ai_class<5 && (latest.ai_conf||0)>=0.5) {
      const names=['健康','白粉病','叶斑病','锈病','虫害'];
      document.getElementById('aiMainText').textContent='⚠ '+names[latest.ai_class]+' ('+((latest.ai_conf||0)*100).toFixed(0)+'%)';
    } else if(latest.ai_class===0) document.getElementById('aiMainText').textContent='植物状态良好 · '+(((latest.ai_conf||0)*100).toFixed(0))+'%';
    else document.getElementById('aiMainText').textContent='等待下一次AI巡检';
    document.getElementById('aiDetail').textContent=latest.timestamp?'最近数据 '+fmtDate(latest.timestamp):'';
    document.getElementById('trendMeta').textContent=`最近${historyHours<24?historyHours+'小时':historyHours===24?'24小时':'7天'} · ${history.length}个显示点`;
    drawCharts(history);
  } catch(e) {
    console.error(e);
    const el=document.getElementById('liveTime'); if(el){el.textContent='● 连接失败';el.className='live-badge offline';}
  } finally { dashboardLoading=false; }
}

async function checkJson(r) { const d=await r.json(); if(!r.ok) throw new Error(d.error||'请求失败'); return d; }

function updatePumps(d) {
  const p1=document.getElementById('pumpWater').querySelector('.pump-dot');
  const p2=document.getElementById('pumpFertA').querySelector('.pump-dot');
  const p3=document.getElementById('pumpFertB').querySelector('.pump-dot');
  const pl=document.getElementById('pumpLight').querySelector('.pump-dot');
  p1.className='pump-dot'+(d.pump1?' on':'');
  p2.className='pump-dot'+(d.pump2?' on':'');
  p3.className='pump-dot'+(d.pump3?' on':'');
  pl.className='pump-dot'+(d.light_pwm>0?' on':'');
  document.querySelector('#pumpWater .pump-text').textContent=d.pump1?'运行中':'关闭';
  document.querySelector('#pumpFertA .pump-text').textContent=d.pump2?'运行中':'关闭';
  document.querySelector('#pumpFertB .pump-text').textContent=d.pump3?'运行中':'关闭';
  document.getElementById('lightVal').textContent=(d.light_pwm?(d.light_pwm/255*100).toFixed(0):0)+'%';
  document.getElementById('slaveCount').textContent = d.slave_count || 0;
  document.getElementById('slvEc').textContent = d.ec ?? '--';
  document.getElementById('slvTemp').textContent = d.soil_temp!=null ? Number(d.soil_temp).toFixed(1) : '--';
  document.getElementById('slvHum').textContent = d.soil_hum!=null ? Number(d.soil_hum).toFixed(1) : '--';
  // 设备页
  ['archPump1','archPump2','archPump3','archLight'].forEach((id,i)=>{
    const el=document.getElementById(id); if(!el) return;
    el.className='arch-pump'+(i<3?(d['pump'+(i+1)]?' on':''):'');
    if(i===3) document.getElementById('archLightVal').textContent=(d.light_pwm?(d.light_pwm/255*100).toFixed(0):0)+'%';
  });
}

function drawCharts(data) {
  if(!data||!data.length) { Object.values(charts).forEach(c=>c.destroy()); charts={}; return; }
  const labels=data.map(d=>formatChartTime(d.timestamp));
  drawChart('chartHealth',labels,[{label:'健康指数',data:data.map(d=>numOrNull(d.health)),color:'#52b788',fill:true}],0,100);
  drawChart('chartEnv',labels,[{label:'温度 °C',data:data.map(d=>numOrNull(d.temperature)),color:'#e76f51',fill:false},{label:'空气湿度 %',data:data.map(d=>numOrNull(d.humidity)),color:'#457b9d',fill:false}]);
  drawChart('chartSoil',labels,[{label:'EC μS/cm',data:data.map(d=>numOrNull(d.ec)),color:'#e9c46a',fill:true},{label:'土壤ADC',data:data.map(d=>numOrNull(d.soil)),color:'#2d6a4f',fill:false,axis:'y1'}]);
}
function drawChart(cid,labels,datasets,min,max) {
  const ctx=document.getElementById(cid)?.getContext('2d'); if(!ctx) return;
  const ds=datasets.map(d=>({label:d.label,data:d.data,borderColor:d.color,backgroundColor:d.fill?d.color+'20':'transparent',fill:d.fill||false,tension:0.15,pointRadius:0,pointHoverRadius:3,borderWidth:1.8,spanGaps:false,yAxisID:d.axis||'y'}));
  const scales={y:{beginAtZero:false,grid:{color:'#f0f0f0'},ticks:{font:{size:10}}},x:{grid:{display:false},ticks:{font:{size:10},maxTicksLimit:8}}};
  if(min!==undefined) scales.y.min=min; if(max!==undefined) scales.y.max=max;
  if(datasets.some(d=>d.axis==='y1')) scales.y1={position:'right',grid:{drawOnChartArea:false},ticks:{font:{size:10}}};
  if(charts[cid]) { charts[cid].data={labels,datasets:ds}; charts[cid].options.scales=scales; charts[cid].update('none'); return; }
  charts[cid]=new Chart(ctx,{type:'line',data:{labels,datasets:ds},options:{responsive:true,maintainAspectRatio:false,animation:false,interaction:{intersect:false,mode:'index'},plugins:{legend:{display:true,position:'top',labels:{boxWidth:12,padding:10,font:{size:11}}}},scales}});
}

function numOrNull(v){ const n=Number(v); return Number.isFinite(n)?n:null; }
function parseDbTime(s){ if(!s) return new Date(0); return new Date(s.replace(' ','T')+'Z'); }
function formatChartTime(s){ const d=parseDbTime(s); return historyHours>=24?d.toLocaleString('zh-CN',{month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'}):d.toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit'}); }

// ======================== 舵机控制 ========================
async function saveServo() {
  if (!requireAuth()) return;
  const pos = parseInt(document.getElementById('servoSlider').value) || 90;
  const ok=await sendDeviceCommand('move',0,pos);
  if(ok) toast('移动命令已发送，约2秒内执行');
}

async function autoFindPlant() {
  startInspection();
}

async function sendDeviceCommand(type,targetId=0,value=0,showLogin=true) {
  if(!token) { if(showLogin) requireAuth(); return false; }
  try {
    const r=await api('/api/device/command',{method:'POST',body:{type,target_id:targetId,value}});
    const d=await r.json();
    if(!r.ok||!d.success){toast(d.error||'命令发送失败');return false;}
    return true;
  } catch(e){toast('网络连接失败');return false;}
}

async function jogServo(type) {
  const ok=await sendDeviceCommand(type,0,180);
  if(ok) toast(type==='jog_left'?'摄像头向左点动':'摄像头向右点动');
}

function stepServo(step){
  const s=document.getElementById('servoSlider');
  s.value=Math.min(160,Math.max(20,parseInt(s.value)+step));
  document.getElementById('servoVal').textContent=s.value;
  sendDeviceCommand('move',0,parseInt(s.value));
}

function startServoState(){
  clearInterval(servoStateTimer);
  loadServoState();
  servoStateTimer=setInterval(loadServoState,2000);
}
async function loadServoState(){
  if(!document.querySelector('#page-device.active')||servoDragging)return;
  try{
    const r=await api('/api/device/status');const d=await r.json();
    const p=Number(d.result?.servo_pos);
    if(Number.isFinite(p)){
      document.getElementById('servoSlider').value=p;
      document.getElementById('servoVal').textContent=p;
    }
  }catch(e){}
}

async function loadTargets() {
  const r=await api('/api/targets');
  plantTargets=await r.json();
  const box=document.getElementById('patrolList');
  box.innerHTML=plantTargets.length?plantTargets.map(t=>`<div class="patrol-item ${t.enabled?'':'disabled'}"><div class="patrol-info"><strong>${esc(t.name)}</strong><span>从机 ${t.slave_addr} · 位置 ${t.servo_pos}</span></div><div class="patrol-actions"><button class="btn btn-primary btn-sm" data-action="visit" data-id="${t.id}">寻植</button><button class="btn btn-ghost btn-sm btn-del" data-action="delete" data-id="${t.id}">删除</button></div></div>`).join(''):'<p class="empty-text">还没有标定种植位。先用点动按钮对准植物，再添加花盆和从机地址。</p>';
}

function onPatrolClick(e){
  const btn=e.target.closest('button[data-action]');
  if(!btn)return;
  const id=parseInt(btn.dataset.id);
  const action=btn.dataset.action;
  e.preventDefault();
  e.stopPropagation();
  if(action==='delete') delPatrolItem(id);
  else if(action==='visit') visitTarget(id);
}
async function delPatrolItem(id){
  if(!token){alert('请先登录后再删除');showAuth('login');return;}
  if(!confirm('确定删除这个种植位？'))return;
  try{
    const r=await api(`/api/targets/${id}`,{method:'DELETE'});
    const d=await r.json();
    if(!r.ok||!d.success){alert('删除失败：'+(d.error||'请重新登录后重试'));showAuth('login');return;}
    loadTargets();toast('已删除');
  }catch(e){alert('网络错误，删除失败');}
}

function renderSlaveOptions(editId=0,selected=2){
  const el=document.getElementById('targetSlave');
  el.innerHTML=Array.from({length:32},(_,i)=>i+2).map(addr=>{
    const used=plantTargets.find(t=>Number(t.slave_addr)===addr&&Number(t.id)!==Number(editId));
    return `<option value="${addr}" ${used?'disabled':''} ${addr===Number(selected)?'selected':''}>从机 ${addr}${used?' · 已绑定 '+esc(used.name):' · 未绑定'}</option>`;
  }).join('');
  document.getElementById('targetSlaveHint').textContent='一台从机只能绑定一个植物';
}

function renderCfgSlaveOptions(selected=0){
  const el=document.getElementById('cfgSlaveSelect');if(!el)return;
  const addr=selected||2;
  el.innerHTML=Array.from({length:32},(_,i)=>i+2).map(a=>`<option value="${a}" ${a===addr?'selected':''}>从机 ${a}</option>`).join('');
  el.onchange=onCfgSlaveChange;
  onCfgSlaveChange();
}
function onCfgSlaveChange(){
  const addr=document.getElementById('cfgSlaveSelect').value;
  const t=plantTargets.find(x=>Number(x.slave_addr)===Number(addr));
  document.getElementById('configPlantName').textContent=' · 从机 '+addr+(t?' ('+esc(t.name)+')':'');
}

function showTargetForm(id=0,presetSlave=0) {
  if(!requireAuth()) return;
  const t=plantTargets.find(x=>x.id===id);
  document.getElementById('targetModal').style.display='flex';
  document.getElementById('targetTitle').textContent=t?'编辑种植位':'添加种植位';
  document.getElementById('targetId').value=t?.id||'';
  document.getElementById('targetName').value=t?.name||'';
  renderSlaveOptions(id,t?.slave_addr||presetSlave||2);
  document.getElementById('targetPos').value=t?.servo_pos||parseInt(document.getElementById('servoSlider').value)||90;
}
function closeTargetForm(){document.getElementById('targetModal').style.display='none';}
function editTarget(id){showTargetForm(id);}
async function saveTarget(){
  const id=document.getElementById('targetId').value;
  const body={name:document.getElementById('targetName').value.trim(),slave_addr:parseInt(document.getElementById('targetSlave').value),servo_pos:parseInt(document.getElementById('targetPos').value),enabled:1};
  if(!body.name||body.slave_addr<2||body.slave_addr>33||body.servo_pos<20||body.servo_pos>160)return toast('请检查名称、从机地址和软件位置');
  const r=await api(id?`/api/targets/${id}`:'/api/targets',{method:id?'PUT':'POST',body});const d=await r.json();
  if(!r.ok||!d.success)return toast(d.error||'保存失败');
  closeTargetForm();loadTargets();toast('种植位标定已保存');
}
async function deleteTarget(id){if(!requireAuth()||!confirm('确定删除这个种植位？'))return;const r=await api(`/api/targets/${id}`,{method:'DELETE'});const d=await r.json();if(!r.ok||!d.success){toast(d.error||'删除失败，请重新登录后重试');return;}loadTargets();toast('已删除');}
async function visitTarget(id){const ok=await sendDeviceCommand('visit',id,0);if(ok)toast('主机将转向该植物并拍照识别');}

async function loadInspection(){
  const r=await api('/api/inspection');const d=await r.json();
  document.getElementById('inspectionEnabled').checked=!!d.enabled;
  document.getElementById('inspectionInterval').value=d.interval_min||30;
  const s=document.getElementById('inspectionState');s.textContent=d.enabled?'自动巡检中':'未启用';s.className='badge'+(d.enabled?' badge-success':'');
}
async function saveInspection(){
  if(!requireAuth())return;
  const body={enabled:document.getElementById('inspectionEnabled').checked,interval_min:parseInt(document.getElementById('inspectionInterval').value)||30};
  const r=await api('/api/inspection',{method:'POST',body});const d=await r.json();if(!r.ok||!d.success)return toast(d.error||'保存失败');loadInspection();toast('自动巡检设置已保存');
}
async function startInspection(){const ok=await sendDeviceCommand('scan');if(ok)toast('已下发整轮自动寻植巡检');}

// ======================== 设备配置 ========================
async function loadDeviceConfig() {
  const r=await api('/api/config'); const c=await r.json();
  const cfgSlave=c.slave_addr||0;
  if(c.soil_target_min) { document.getElementById('cfgSoilMin').value=c.soil_target_min; document.getElementById('cfgSoilMax').value=c.soil_target_max; document.getElementById('cfgEcMin').value=c.ec_growth_min; document.getElementById('cfgEcMax').value=c.ec_growth_max; document.getElementById('cfgWaterCd').value=c.watering_cooldown; document.getElementById('cfgFertCd').value=c.fertilizer_cooldown; document.getElementById('cfgLightMin').value=c.light_min||1600; document.getElementById('cfgLightPwm').value=c.light_pwm||180; document.getElementById('cfgAge').value=c.plant_age_days||30; document.getElementById('cfgPlantName').value=c.plant_name||''; document.getElementById('configPlantName').textContent=c.plant_name?' · '+c.plant_name:''; }
  // also load pump status
  const latest=await api('/api/latest').then(r=>r.json());
  if(latest.temperature!==undefined) updatePumps(latest);
  await Promise.all([loadTargets(),loadInspection()]);
  renderCfgSlaveOptions(cfgSlave);
}
async function saveConfig() {
  if(!requireAuth()) return;
  const body={slave_addr:parseInt(document.getElementById('cfgSlaveSelect')?.value)||2,plant_name:document.getElementById('cfgPlantName').value.trim(),plant_age_days:parseInt(document.getElementById('cfgAge').value)||30,soil_min:parseInt(document.getElementById('cfgSoilMin').value)||1800,soil_max:parseInt(document.getElementById('cfgSoilMax').value)||2500,ec_min:parseInt(document.getElementById('cfgEcMin').value)||150,ec_max:parseInt(document.getElementById('cfgEcMax').value)||300,watering_cd:parseInt(document.getElementById('cfgWaterCd').value)||5,fert_cd:parseInt(document.getElementById('cfgFertCd').value)||60,light_min:parseInt(document.getElementById('cfgLightMin').value)||1600,light_pwm:parseInt(document.getElementById('cfgLightPwm').value)||180};
  const r=await api('/api/config',{method:'POST',body});
  const d=await r.json();
  const s=document.getElementById('configStatus');
  if(!r.ok||!d.success){s.textContent='保存失败';s.className='badge badge-error';return toast(d.error||'配置保存失败');}
  s.textContent='服务端已保存'; s.className='badge badge-success';
  toast(d.message||'配置已保存');
}

// ======================== 方案 ========================
async function loadStrategies() {
  if (currentTab === 'mine' && !currentUser) {
    document.getElementById('strategyList').innerHTML = '<p style="color:#888;padding:40px;text-align:center">请先登录，才能查看和创建养护方案 (｀・ω・´)<br><br><button class="btn btn-primary btn-sm" onclick="showAuth(\'login\')">去登录</button></p>';
    return;
  }
  const r = await api('/api/strategies?mine=' + (currentTab === 'mine' ? '1' : '0'));
  const list = await r.json();
  const c = document.getElementById('strategyList');
  if (!list.length) {
    c.innerHTML = currentTab === 'mine'
      ? '<p style="color:#888;padding:40px;text-align:center;">还没有创建方案，去方案广场看看别人的吧 (。・ω・。)</p>'
      : '<p style="color:#888;padding:40px;text-align:center;">方案广场空空如也，来创建第一个方案吧 (。・ω・。)</p>';
    return;
  }
  c.innerHTML = list.map(s => `<div class="strategy-card"><div class="strat-header"><span class="strat-plant">${esc(s.plant_name)}</span><span class="strat-stars">★ ${s.stars||0}</span></div><div class="strat-author">by ${esc(s.username||'系统预置')} · ${fmtDate(s.created_at)}</div><div class="strat-desc">${esc(s.description||'暂无简介')}</div><div class="strat-params"><span>土壤: <strong>${s.soil_min}-${s.soil_max}</strong></span><span>EC: <strong>${s.ec_min}-${s.ec_max}</strong></span><span>浇水: <strong>${s.watering_cd}分</strong></span><span>施肥: <strong>${s.fert_cd}分</strong></span></div><div class="strat-actions"><select class="apply-slave-select" id="applySlave_${s.id}">${Array.from({length:32},(_,i)=>i+2).map(addr=>`<option value="${addr}">从机 ${addr}</option>`).join('')}</select><button class="btn btn-primary btn-sm" onclick="applyStrategy(${s.id})">下发到设备</button>${currentUser&&currentUser.id===s.user_id?`<button class="btn btn-ghost btn-sm" onclick="editStrategy(${s.id})">编辑</button><button class="btn btn-ghost btn-sm" onclick="deleteStrategy(${s.id})">删除</button>`:''}</div></div>`).join('');
}
function showStrategyForm(id) {
  if(!requireAuth()) return;
  document.getElementById('strategyModal').style.display='flex';
  if(id) {
    document.getElementById('strategyModalTitle').textContent='编辑方案';
    api('/api/strategies?mine=1').then(r=>r.json()).then(list=>{ const s=list.find(x=>x.id===id); if(s) { document.getElementById('stratId').value=s.id; document.getElementById('stratName').value=s.plant_name; document.getElementById('stratDesc').value=s.description||''; document.getElementById('stratSoilMin').value=s.soil_min; document.getElementById('stratSoilMax').value=s.soil_max; document.getElementById('stratEcMin').value=s.ec_min; document.getElementById('stratEcMax').value=s.ec_max; document.getElementById('stratWaterCd').value=s.watering_cd; document.getElementById('stratFertCd').value=s.fert_cd; document.getElementById('stratPublic').checked=!!s.is_public; } });
  } else { document.getElementById('strategyModalTitle').textContent='创建养护方案'; document.getElementById('stratId').value=''; document.getElementById('stratName').value=''; document.getElementById('stratDesc').value=''; }
}
function closeStrategyForm() { document.getElementById('strategyModal').style.display='none'; }
async function saveStrategy() {
  const id=document.getElementById('stratId').value;
  const body={plant_name:document.getElementById('stratName').value.trim(),description:document.getElementById('stratDesc').value.trim(),soil_min:parseInt(document.getElementById('stratSoilMin').value)||1800,soil_max:parseInt(document.getElementById('stratSoilMax').value)||2500,ec_min:parseInt(document.getElementById('stratEcMin').value)||150,ec_max:parseInt(document.getElementById('stratEcMax').value)||300,watering_cd:parseInt(document.getElementById('stratWaterCd').value)||5,fert_cd:parseInt(document.getElementById('stratFertCd').value)||60,is_public:document.getElementById('stratPublic').checked?1:0};
  if(!body.plant_name) return toast('请输入植物名称');
  const r=await api(id?`/api/strategies/${id}`:'/api/strategies',{method:id?'PUT':'POST',body});
  const d=await r.json(); if(d.error) return toast(d.error);
  closeStrategyForm(); loadStrategies(); toast(id?'方案已更新':'方案创建成功');
}
function editStrategy(id) { showStrategyForm(id); }
async function deleteStrategy(id) { if(!confirm('确定删除？')) return; await api(`/api/strategies/${id}`,{method:'DELETE'}); loadStrategies(); toast('已删除'); }
async function applyStrategy(id) { if(!requireAuth()) return; const slaveEl=document.getElementById(`applySlave_${id}`); const slave_addr=slaveEl?parseInt(slaveEl.value):0; const r=await api(`/api/strategies/${id}/apply`,{method:'POST',body:{slave_addr}}); const d=await r.json(); toast(d.message||'已下发'); loadStrategies(); }

// ======================== 相册 + 延时 ========================
async function loadGallery() {
  const r=await api('/api/images?limit=100');
  const images=(await r.json()).filter(x=>x.filename);
  timelapsePhotos=[...images].reverse();
  const grid=document.getElementById('imageGrid');
  if(!images.length) { grid.innerHTML='<p class="gallery-empty">还没有照片，完成一次AI巡检后会显示在这里。</p>'; return; }
  grid.innerHTML=images.map(img=>`<div class="image-card" onclick="openImage('${imageUrl(img.filename)}')"><button class="image-delete" onclick="event.stopPropagation();deleteImage(${img.id})" title="删除">×</button><img src="${imageUrl(img.filename)}" alt="植物照片" loading="lazy" onerror="imageFail(this)"><div class="image-info"><strong>健康 ${img.health||0}%</strong><span>${fmtDate(img.timestamp)}</span></div></div>`).join('');
}

async function deleteImage(id) {
  if (!requireAuth()) return;
  if (!confirm('确定删除这张照片？')) return;
  const r = await api(`/api/images/${id}`, { method: 'DELETE' });
  const d = await r.json();
  if (!r.ok || !d.success) { toast(d.error || '删除失败'); return; }
  loadGallery(); toast('已删除');
}

function imageUrl(name){return '/uploads/'+encodeURIComponent(name);}
function imageFail(img){img.onerror=null;img.src='data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 320 240"><rect fill="%23edf2ef" width="320" height="240"/><text x="160" y="125" text-anchor="middle" fill="%23889990" font-size="16">图片不可用</text></svg>';}
function openImage(src){window.open(src,'_blank');}

function renderTimelapseFrame(i){
  const p=timelapsePhotos[i];if(!p)return;
  const img=document.getElementById('timelapseImg');img.style.display='block';img.src=imageUrl(p.filename);img.onerror=()=>imageFail(img);
  document.getElementById('timelapseEmpty').style.display='none';
  document.getElementById('timelapseSlider').value=i;
  document.getElementById('timelapseInfo').textContent=(i+1)+' / '+timelapsePhotos.length+' · '+fmtDate(p.timestamp);
}

function toggleTimelapse() {
  const player=document.getElementById('timelapsePlayer');
  if(player.style.display==='none') {
    if(!timelapsePhotos.length) return toast('没有照片可以播放');
    player.style.display='block';
    document.getElementById('btnTimelapse').textContent='■ 停止';
    timelapseIdx=0;
    document.getElementById('timelapseSlider').max=timelapsePhotos.length-1;
    renderTimelapseFrame(0);
    timelapseIdx=1;
    playTimelapse();
  } else {
    stopTimelapse(false);
  }
}
function playTimelapse() {
  clearInterval(timelapseTimer);
  timelapsePlaying=true;
  const speed=parseInt(document.getElementById('timelapseSpeed').value)||200;
  timelapseTimer=setInterval(() => {
    if(timelapseIdx>=timelapsePhotos.length) { clearInterval(timelapseTimer);timelapsePlaying=false;document.getElementById('btnTimelapse').textContent='↻ 重新播放';return; }
    renderTimelapseFrame(timelapseIdx);
    timelapseIdx++;
  }, speed);
}
function stopTimelapse(hide=false){clearInterval(timelapseTimer);timelapsePlaying=false;document.getElementById('btnTimelapse').textContent='▶ 延时播放';if(hide)document.getElementById('timelapsePlayer').style.display='none';else document.getElementById('timelapsePlayer').style.display='none';}
function seekTimelapse(v) { timelapseIdx=parseInt(v);renderTimelapseFrame(timelapseIdx);timelapseIdx++; }
function changeSpeed() { if(timelapsePlaying) playTimelapse(); }

// ======================== 工具 ========================
function toast(msg) { const el=document.getElementById('toast'); el.textContent=msg; el.style.display='block'; clearTimeout(el._t); el._t=setTimeout(()=>{el.style.display='none';},2500); }
function esc(s) { if(!s) return ''; return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'); }
function fmtDate(s) { if(!s) return ''; return parseDbTime(s).toLocaleString('zh-CN',{month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'}); }
