/*
  小花园 - 服务端
  预置养护方案 + 延时视频API + 设备主从架构
*/
const express = require('express');
const multer = require('multer');
const path = require('path');
const fs = require('fs');
const cors = require('cors');
const crypto = require('crypto');
const jwt = require('jsonwebtoken');
const bcrypt = require('bcryptjs');
const initSqlJs = require('sql.js');

const app = express();
const PORT = process.env.PORT || 3000;
const JWT_SECRET_FILE = path.join(__dirname, '.jwt_secret');
let JWT_SECRET = process.env.JWT_SECRET || '';
if(!JWT_SECRET) {
  try { JWT_SECRET = fs.readFileSync(JWT_SECRET_FILE, 'utf8').trim(); } catch(e) {}
  if(!JWT_SECRET) { JWT_SECRET = crypto.randomBytes(32).toString('hex'); fs.writeFileSync(JWT_SECRET_FILE, JWT_SECRET); }
}
const DB_PATH = path.join(__dirname, 'plant.db');

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));
app.use('/uploads', express.static(path.join(__dirname, 'uploads')));

const uploadDir = path.join(__dirname, 'uploads');
if (!fs.existsSync(uploadDir)) fs.mkdirSync(uploadDir);

const storage = multer.diskStorage({
  destination: (req, file, cb) => cb(null, uploadDir),
  filename: (req, file, cb) => {
    const d = new Date();
    const ts = `${d.getFullYear()}${pad(d.getMonth()+1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}_${d.getMilliseconds().toString().padStart(3,'0')}`;
    cb(null, `plant_${ts}_${crypto.randomBytes(2).toString('hex')}.jpg`);
  }
});
const upload = multer({ storage, limits: { fileSize: 5*1024*1024 } });

function pad(n) { return n.toString().padStart(2,'0'); }

// ======================== 数据库 ========================
let db;
function dbRun(sql, params=[]) { db.run(sql, params); saveDB(); }
function dbGet(sql, params=[]) { const s=db.prepare(sql); if(params.length)s.bind(params); const r=s.step()?s.getAsObject():null; s.free(); return r; }
function dbAll(sql, params=[]) { const s=db.prepare(sql); if(params.length)s.bind(params); const rows=[]; while(s.step())rows.push(s.getAsObject()); s.free(); return rows; }
function dbLastID() { return dbGet("SELECT last_insert_rowid() as id")?.id||0; }
function dbInsert(sql, params=[]) { db.run(sql,params); const id=dbLastID(); saveDB(); return id; }
function saveDB() { try { fs.writeFileSync(DB_PATH, Buffer.from(db.export())); } catch(e) {} }

async function initDB() {
  const SQL = await initSqlJs();
  db = fs.existsSync(DB_PATH) ? new SQL.Database(fs.readFileSync(DB_PATH)) : new SQL.Database();

  db.run(`CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT UNIQUE NOT NULL, password TEXT NOT NULL, created_at DATETIME DEFAULT CURRENT_TIMESTAMP)`);
  db.run(`CREATE TABLE IF NOT EXISTS sensor_data (id INTEGER PRIMARY KEY AUTOINCREMENT, temperature REAL, humidity REAL, light INTEGER, soil INTEGER, ec INTEGER, health INTEGER, ai_class INTEGER, ai_conf REAL, stage INTEGER, stage_name TEXT, trend REAL, soil_temp REAL, soil_hum REAL, pump1 INTEGER DEFAULT 0, pump2 INTEGER DEFAULT 0, pump3 INTEGER DEFAULT 0, light_pwm INTEGER DEFAULT 0, slave_count INTEGER DEFAULT 0, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP)`);
  db.run(`CREATE TABLE IF NOT EXISTS images (id INTEGER PRIMARY KEY AUTOINCREMENT, filename TEXT, health INTEGER, stage INTEGER, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP)`);
  db.run(`CREATE TABLE IF NOT EXISTS device_config (id INTEGER PRIMARY KEY CHECK(id=1), plant_age_days INTEGER DEFAULT 30, soil_min INTEGER DEFAULT 1800, soil_max INTEGER DEFAULT 2500, ec_min INTEGER DEFAULT 150, ec_max INTEGER DEFAULT 300, watering_cd INTEGER DEFAULT 5, fert_cd INTEGER DEFAULT 60, plant_name TEXT DEFAULT '', servo_angle INTEGER DEFAULT 90, updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)`);
  db.run(`CREATE TABLE IF NOT EXISTS strategies (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL DEFAULT 0, plant_name TEXT NOT NULL, description TEXT DEFAULT '', soil_min INTEGER DEFAULT 1800, soil_max INTEGER DEFAULT 2500, ec_min INTEGER DEFAULT 150, ec_max INTEGER DEFAULT 300, watering_cd INTEGER DEFAULT 5, fert_cd INTEGER DEFAULT 60, is_public INTEGER DEFAULT 1, stars INTEGER DEFAULT 0, created_at DATETIME DEFAULT CURRENT_TIMESTAMP)`);
  db.run(`CREATE TABLE IF NOT EXISTS plant_targets (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, slave_addr INTEGER NOT NULL, servo_pos INTEGER NOT NULL DEFAULT 90, enabled INTEGER DEFAULT 1)`);
  db.run(`CREATE TABLE IF NOT EXISTS device_commands (id INTEGER PRIMARY KEY AUTOINCREMENT, type TEXT NOT NULL, target_id INTEGER, value INTEGER DEFAULT 0, status TEXT DEFAULT 'pending', result TEXT DEFAULT '', created_at DATETIME DEFAULT CURRENT_TIMESTAMP, finished_at DATETIME)`);
  db.run(`CREATE TABLE IF NOT EXISTS inspection_config (id INTEGER PRIMARY KEY CHECK(id=1), enabled INTEGER DEFAULT 0, interval_min INTEGER DEFAULT 30, next_run INTEGER DEFAULT 0)`);

  if (!dbGet("SELECT * FROM device_config WHERE id=1")) db.run("INSERT INTO device_config(id) VALUES(1)");
  if (!dbGet("SELECT * FROM inspection_config WHERE id=1")) db.run("INSERT INTO inspection_config(id) VALUES(1)");
  const cfgCols = dbAll('PRAGMA table_info(device_config)').map(x=>x.name);
  if(!cfgCols.includes('light_min')) db.run('ALTER TABLE device_config ADD COLUMN light_min INTEGER DEFAULT 1600');
  if(!cfgCols.includes('light_pwm')) db.run('ALTER TABLE device_config ADD COLUMN light_pwm INTEGER DEFAULT 180');
  if(!cfgCols.includes('servo_angle')) db.run('ALTER TABLE device_config ADD COLUMN servo_angle INTEGER DEFAULT 90');
  if(!cfgCols.includes('target_slave')) db.run('ALTER TABLE device_config ADD COLUMN target_slave INTEGER DEFAULT 0');
  try { db.run('CREATE UNIQUE INDEX IF NOT EXISTS idx_plant_targets_slave ON plant_targets(slave_addr)'); } catch(e) {}

  db.run("DELETE FROM strategies WHERE user_id=0");
  const now = new Date().toISOString().replace('T',' ').substring(0,19);
  const presetSQL = `
    INSERT OR IGNORE INTO strategies (user_id,plant_name,description,soil_min,soil_max,ec_min,ec_max,watering_cd,fert_cd,is_public,stars,created_at) VALUES
    (0,'番茄','喜光喜温，幼苗期多氮肥，开花后补磷钾。土壤保持湿润但别积水',2000,2800,200,350,5,60,1,42,'${now}'),
    (0,'月季','喜光耐旱，开花期需磷钾肥。土表干了再浇透，太湿容易烂根',1500,2200,150,300,10,120,1,38,'${now}'),
    (0,'多肉','耐旱怕涝，宁干勿湿！土完全干了再浇。几乎不需要施肥',800,1500,50,150,1440,10080,1,55,'${now}'),
    (0,'绿萝','耐阴好养，保持土壤微湿就好。偶尔补点氮肥叶子更绿',1600,2400,100,200,15,10080,1,31,'${now}'),
    (0,'辣椒','喜光喜热，结果期需钾肥多。光照不够会掉花',2200,3000,250,400,5,60,1,47,'${now}')
  `;
  db.exec(presetSQL);
  saveDB();
  console.log('已预置5套养护方案');
}

// ======================== JWT ========================
function auth(req, res, next) {
  const t = req.headers.authorization?.replace('Bearer ','');
  if(!t) return res.status(401).json({error:'请先登录'});
  try { req.user = jwt.verify(t, JWT_SECRET); next(); } catch(e) { res.status(401).json({error:'登录已过期'}); }
}

// ======================== 认证 ========================
app.post('/api/auth/register', (req,res) => {
  const {username,password}=req.body;
  if(!username||!password||password.length<4) return res.status(400).json({error:'用户名和密码不能为空，密码至少4位'});
  try {
    const id=dbInsert('INSERT INTO users(username,password) VALUES(?,?)',[username,bcrypt.hashSync(password,10)]);
    const token=jwt.sign({id,username},JWT_SECRET,{expiresIn:'30d'});
    res.json({token,user:{id,username}});
  } catch(e) { res.status(400).json({error:'用户名已被注册'}); }
});
app.post('/api/auth/login', (req,res) => {
  const u=dbGet('SELECT * FROM users WHERE username=?',[req.body.username]);
  if(!u||!bcrypt.compareSync(req.body.password,u.password)) return res.status(401).json({error:'用户名或密码错误'});
  res.json({token:jwt.sign({id:u.id,username:u.username},JWT_SECRET,{expiresIn:'30d'}),user:{id:u.id,username:u.username}});
});
app.get('/api/auth/me', auth, (req,res) => res.json(dbGet('SELECT id,username,created_at FROM users WHERE id=?',[req.user.id])||{}));

// ======================== ESP32上传 ========================
app.post('/api/upload', upload.single('image'), (req,res) => {
  try {
    const d = typeof req.body.data === 'string' ? JSON.parse(req.body.data) : req.body;
    if (!d || typeof d !== 'object') return res.status(400).json({error:'没有收到设备数据'});
    dbRun(`INSERT INTO sensor_data(temperature,humidity,light,soil,ec,health,ai_class,ai_conf,stage,stage_name,trend,soil_temp,soil_hum,pump1,pump2,pump3,light_pwm,slave_count) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
      [d.temp||0,d.humidity||0,d.light||0,d.soil||0,d.ec||0,d.health||0,d.ai_class||0,d.ai_conf||0,d.stage||0,d.stage_name||'',d.trend||0,d.soil_temp||0,d.soil_hum||0,d.pump1?1:0,d.pump2?1:0,d.pump3?1:0,d.light_pwm||0,d.slave_count||0]);
    if(req.file) dbRun('INSERT INTO images(filename,health,stage) VALUES(?,?,?)',[req.file.filename,d.health,d.stage]);
    res.json({success:true});
  } catch(e) {
    console.error('upload error:', e);
    res.status(500).json({error:'数据处理失败',detail:e.message});
  }
});

// ======================== 配置 ========================
app.get('/api/config', (req,res) => {
  const r=dbGet('SELECT * FROM device_config WHERE id=1');
  res.json(r?{plant_age_days:r.plant_age_days,plant_name:r.plant_name||'',soil_target_min:r.soil_min,soil_target_max:r.soil_max,ec_growth_min:r.ec_min,ec_growth_max:r.ec_max,watering_cooldown:r.watering_cd,fertilizer_cooldown:r.fert_cd,light_min:r.light_min||1600,light_pwm:r.light_pwm||180,servo_angle:r.servo_angle||90,servo_auto:0}:{});
});
app.post('/api/config', auth, (req,res) => {
  const c=req.body;
  const old=dbGet('SELECT * FROM device_config WHERE id=1')||{};
  dbRun(`UPDATE device_config SET plant_age_days=?,soil_min=?,soil_max=?,ec_min=?,ec_max=?,watering_cd=?,fert_cd=?,plant_name=?,servo_angle=?,light_min=?,light_pwm=?,updated_at=CURRENT_TIMESTAMP WHERE id=1`,
    [c.plant_age_days??old.plant_age_days,c.soil_min??old.soil_min,c.soil_max??old.soil_max,c.ec_min??old.ec_min,c.ec_max??old.ec_max,c.watering_cd??old.watering_cd,c.fert_cd??old.fert_cd,c.plant_name??old.plant_name,c.servo_angle??old.servo_angle,c.light_min??old.light_min,c.light_pwm??old.light_pwm]);
  res.json({success:true,message:'配置已保存，设备下次同步后生效'});
});

// ======================== 种植位标定和寻植 ========================
app.get('/api/targets', (req,res) => res.json(dbAll('SELECT * FROM plant_targets ORDER BY id')));
function targetData(t, old={}) {
  const name=String(t.name??old.name??'').trim();
  const slave=Number(t.slave_addr??old.slave_addr);
  const pos=Number(t.servo_pos??old.servo_pos??90);
  return {name,slave,pos,enabled:(t.enabled??old.enabled??1)?1:0};
}
app.post('/api/targets', auth, (req,res) => {
  const t=targetData(req.body);
  const used=dbGet('SELECT id,name FROM plant_targets WHERE slave_addr=?',[t.slave]);
  if(used) return res.status(409).json({error:`从机${t.slave}已绑定「${used.name}」`});
  const id=dbInsert('INSERT INTO plant_targets(name,slave_addr,servo_pos,enabled) VALUES(?,?,?,?)',
    [t.name,t.slave,t.pos,t.enabled]);
  res.json({success:true,id});
});
app.put('/api/targets/:id', auth, (req,res) => {
  const old=dbGet('SELECT * FROM plant_targets WHERE id=?',[req.params.id]);
  if(!old) return res.status(404).json({error:'种植位不存在'});
  const t=targetData(req.body,old);
  const used=dbGet('SELECT id,name FROM plant_targets WHERE slave_addr=? AND id<>?',[t.slave,req.params.id]);
  if(used) return res.status(409).json({error:`从机${t.slave}已绑定「${used.name}」`});
  dbRun('UPDATE plant_targets SET name=?,slave_addr=?,servo_pos=?,enabled=? WHERE id=?',
    [t.name,t.slave,t.pos,t.enabled,req.params.id]);
  res.json({success:true});
});
app.delete('/api/targets/:id', (req,res) => {
  const t = req.headers.authorization?.replace('Bearer ','');
  if(!t) return res.status(401).json({error:'请先登录'});
  try { jwt.verify(t, JWT_SECRET); } catch(e) { return res.status(401).json({error:'登录已过期'}); }
  dbRun('DELETE FROM plant_targets WHERE id=?',[req.params.id]);
  res.json({success:true});
});

app.get('/api/targets/slaves', (req,res) => {
  const targets = dbAll('SELECT id, name, slave_addr FROM plant_targets');
  const map = {};
  targets.forEach(t => { map[t.slave_addr] = {id:t.id, name:t.name}; });
  const result = [];
  for(let addr=2; addr<=33; addr++) {
    result.push({addr, name: map[addr]?.name||null, id: map[addr]?.id||null});
  }
  res.json(result);
});

function addCommand(type,targetId=0,value=0) {
  if(type==='move') {
    const old=dbGet("SELECT id FROM device_commands WHERE type='move' AND status='pending' ORDER BY id DESC LIMIT 1");
    if(old) { dbRun('UPDATE device_commands SET value=?,created_at=CURRENT_TIMESTAMP WHERE id=?',[value,old.id]); return old.id; }
  }
  return dbInsert('INSERT INTO device_commands(type,target_id,value) VALUES(?,?,?)',[type,targetId||null,value||0]);
}
app.post('/api/device/command', auth, (req,res) => {
  const {type,target_id,value}=req.body;
  if(!['move','jog_left','jog_right','visit','scan','stop'].includes(type)) return res.status(400).json({error:'不支持的命令'});
  res.json({success:true,id:addCommand(type,target_id,value)});
});
app.get('/api/device/command', (req,res) => {
  db.run("UPDATE device_commands SET status='pending' WHERE status='running' AND datetime(created_at)<datetime('now','-2 minutes')");
  const auto=dbGet('SELECT * FROM inspection_config WHERE id=1')||{};
  const now=Date.now();
  if(auto.enabled && (!auto.next_run || now>=auto.next_run) && !dbGet("SELECT id FROM device_commands WHERE status IN ('pending','running')")) {
    addCommand('scan');
    dbRun('UPDATE inspection_config SET next_run=? WHERE id=1',[now+Math.max(auto.interval_min||30,5)*60000]);
  }
  const cmd=dbGet("SELECT * FROM device_commands WHERE status='pending' ORDER BY id LIMIT 1");
  if(!cmd) return res.json({});
  dbRun("UPDATE device_commands SET status='running' WHERE id=?",[cmd.id]);
  if(cmd.target_id) cmd.target=dbGet('SELECT * FROM plant_targets WHERE id=?',[cmd.target_id]);
  if(cmd.type==='scan') cmd.targets=dbAll('SELECT * FROM plant_targets WHERE enabled=1 ORDER BY id');
  res.json(cmd);
});
app.get('/api/device/status', (req,res) => {
  const cmd=dbGet("SELECT * FROM device_commands WHERE status IN ('done','failed') ORDER BY id DESC LIMIT 1")||{};
  if(cmd.result) { try { cmd.result=JSON.parse(cmd.result); } catch(e){} }
  res.json(cmd);
});
app.post('/api/device/command/:id/ack', (req,res) => {
  dbRun('UPDATE device_commands SET status=?,result=?,finished_at=CURRENT_TIMESTAMP WHERE id=?',
    [req.body.success===false?'failed':'done',JSON.stringify(req.body||{}),req.params.id]);
  res.json({success:true});
});
app.get('/api/inspection', (req,res) => res.json(dbGet('SELECT * FROM inspection_config WHERE id=1')||{}));
app.post('/api/inspection', auth, (req,res) => {
  const enabled=req.body.enabled?1:0, interval=Math.min(Math.max(Number(req.body.interval_min)||30,5),1440);
  dbRun('UPDATE inspection_config SET enabled=?,interval_min=?,next_run=? WHERE id=1',[enabled,interval,Date.now()+interval*60000]);
  res.json({success:true});
});

// ======================== 数据查询 ========================
app.get('/api/latest', (req,res) => res.json(dbGet('SELECT * FROM sensor_data ORDER BY timestamp DESC,id DESC LIMIT 1')||{}));
app.get('/api/history', (req,res) => {
  const h=Math.min(Math.max(parseInt(req.query.hours)||24,1),720);
  const maxPoints=Math.min(Math.max(parseInt(req.query.points)||240,48),500);
  const rows=dbAll(`SELECT * FROM sensor_data WHERE datetime(timestamp)>=datetime('now','-${h} hours') ORDER BY timestamp ASC,id ASC`);
  if(rows.length<=maxPoints) return res.json(rows);
  const size=Math.ceil(rows.length/maxPoints), out=[];
  for(let i=0;i<rows.length;i+=size) {
    const part=rows.slice(i,i+size), last=part[part.length-1];
    const avg=name=>{ const v=part.map(x=>Number(x[name])).filter(Number.isFinite); return v.length?v.reduce((a,b)=>a+b,0)/v.length:null; };
    out.push({...last,temperature:avg('temperature'),humidity:avg('humidity'),light:avg('light'),soil:avg('soil'),ec:avg('ec'),health:avg('health'),soil_temp:avg('soil_temp'),soil_hum:avg('soil_hum')});
  }
  res.json(out);
});
app.get('/api/images', (req,res) => {
  const limit=Math.min(Math.max(parseInt(req.query.limit)||100,1),300);
  res.json(dbAll("SELECT * FROM images WHERE filename IS NOT NULL AND filename<>'' ORDER BY timestamp DESC,id DESC LIMIT ?",[limit]));
});
app.delete('/api/images/:id', (req,res) => {
  const t = req.headers.authorization?.replace('Bearer ','');
  if(!t) return res.status(401).json({error:'请先登录'});
  try { jwt.verify(t, JWT_SECRET); } catch(e) { return res.status(401).json({error:'登录已过期'}); }
  const img = dbGet('SELECT filename FROM images WHERE id=?',[req.params.id]);
  if(!img) return res.status(404).json({error:'图片不存在'});
  if(img.filename) { try { fs.unlinkSync(path.join(uploadDir, img.filename)); } catch(e) {} }
  dbRun('DELETE FROM images WHERE id=?',[req.params.id]);
  res.json({success:true});
});
app.get('/api/stats', (req,res) => {
  const today=dbGet("SELECT COUNT(*) as cnt,AVG(health) as avg_health,MAX(health) as max_h,MIN(health) as min_h FROM sensor_data WHERE date(timestamp)=date('now')")||{};
  const week=dbGet("SELECT AVG(temperature) as avg_temp,AVG(humidity) as avg_hum,AVG(ec) as avg_ec,AVG(soil) as avg_soil FROM sensor_data WHERE datetime(timestamp)>=datetime('now','-7 days')")||{};
  const total=dbGet('SELECT COUNT(*) as total FROM sensor_data')||{};
  res.json({today,week,total:total.total||0});
});

// ======================== 策略 ========================
app.get('/api/strategies', (req,res) => {
  const mine=req.query.mine==='1';
  let uid=0; try { const t=req.headers.authorization?.replace('Bearer ',''); if(t) uid=jwt.verify(t,JWT_SECRET).id; } catch(e){}
  const sql=mine&&uid ? 'SELECT s.*,u.username FROM strategies s LEFT JOIN users u ON s.user_id=u.id WHERE s.user_id=? ORDER BY s.created_at DESC'
    : 'SELECT s.*,u.username FROM strategies s LEFT JOIN users u ON s.user_id=u.id WHERE s.is_public=1 ORDER BY s.stars DESC,s.created_at DESC';
  res.json(dbAll(sql, mine&&uid?[uid]:[]));
});
app.post('/api/strategies', auth, (req,res) => {
  const s=req.body; if(!s.plant_name) return res.status(400).json({error:'请输入植物名称'});
  dbRun(`INSERT INTO strategies(user_id,plant_name,description,soil_min,soil_max,ec_min,ec_max,watering_cd,fert_cd,is_public) VALUES(?,?,?,?,?,?,?,?,?,?)`,
    [req.user.id,s.plant_name,s.description||'',s.soil_min||1800,s.soil_max||2500,s.ec_min||150,s.ec_max||300,s.watering_cd||5,s.fert_cd||60,s.is_public!==0?1:0]);
  res.json({id:dbLastID(),message:'方案创建成功'});
});
app.put('/api/strategies/:id', auth, (req,res) => {
  const s=req.body;
  dbRun(`UPDATE strategies SET plant_name=?,description=?,soil_min=?,soil_max=?,ec_min=?,ec_max=?,watering_cd=?,fert_cd=?,is_public=? WHERE id=? AND user_id=?`,
    [s.plant_name,s.description||'',s.soil_min||1800,s.soil_max||2500,s.ec_min||150,s.ec_max||300,s.watering_cd||5,s.fert_cd||60,s.is_public!==0?1:0,req.params.id,req.user.id]);
  res.json({success:true});
});
app.delete('/api/strategies/:id', auth, (req,res) => {
  dbRun('DELETE FROM strategies WHERE id=? AND user_id=?',[req.params.id,req.user.id]); res.json({success:true});
});
app.post('/api/strategies/:id/apply', auth, (req,res) => {
  const s=dbGet('SELECT * FROM strategies WHERE id=?',[req.params.id]);
  if(!s) return res.status(404).json({error:'方案不存在'});
  dbRun(`UPDATE device_config SET plant_age_days=30,soil_min=?,soil_max=?,ec_min=?,ec_max=?,watering_cd=?,fert_cd=?,plant_name=?,updated_at=CURRENT_TIMESTAMP WHERE id=1`,
    [s.soil_min,s.soil_max,s.ec_min,s.ec_max,s.watering_cd,s.fert_cd,s.plant_name]);
  dbRun('UPDATE strategies SET stars=stars+1 WHERE id=?',[req.params.id]);
  res.json({success:true,message:`已应用「${s.plant_name}」养护方案`});
});
app.post('/api/strategies/:id/apply-to', auth, (req,res) => {
  const s=dbGet('SELECT * FROM strategies WHERE id=?',[req.params.id]);
  if(!s) return res.status(404).json({error:'方案不存在'});
  const slave=Number(req.body.slave_addr);
  if(!slave||slave<2||slave>33) return res.status(400).json({error:'无效的从机地址'});
  dbRun(`UPDATE device_config SET plant_age_days=30,soil_min=?,soil_max=?,ec_min=?,ec_max=?,watering_cd=?,fert_cd=?,plant_name=?,target_slave=?,updated_at=CURRENT_TIMESTAMP WHERE id=1`,
    [s.soil_min,s.soil_max,s.ec_min,s.ec_max,s.watering_cd,s.fert_cd,s.plant_name,slave]);
  res.json({success:true,message:`已应用「${s.plant_name}」方案到从机${slave}`});
});

app.get('/api/slaves/config', (req,res) => {
  const addr=Number(req.query.addr);
  if(!addr||addr<2||addr>33) return res.status(400).json({error:'无效的从机地址'});
  const cfg=dbGet('SELECT * FROM device_config WHERE id=1');
  if(cfg && cfg.target_slave === addr) {
    res.json({
      target_slave: cfg.target_slave,
      plant_name: cfg.plant_name||'',
      soil_min: cfg.soil_min,
      soil_max: cfg.soil_max,
      ec_min: cfg.ec_min,
      ec_max: cfg.ec_max,
      watering_cd: cfg.watering_cd,
      fert_cd: cfg.fert_cd,
      plant_age_days: cfg.plant_age_days,
      servo_angle: cfg.servo_angle||90,
      light_min: cfg.light_min||1600,
      light_pwm: cfg.light_pwm||180
    });
  } else {
    res.json({
      target_slave: 0,
      plant_name: '',
      soil_min: 1800,
      soil_max: 2500,
      ec_min: 150,
      ec_max: 300,
      watering_cd: 5,
      fert_cd: 60,
      plant_age_days: 30,
      servo_angle: 90,
      light_min: 1600,
      light_pwm: 180
    });
  }
});

initDB().then(() => app.listen(PORT, () => console.log(`小花园已启动 http://localhost:${PORT}`)));
