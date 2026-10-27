/*
 * 文件作用：运行电脑端遥测模拟服务，向共用网页提供状态和故障控制接口。
 * 包含模块：CRC16、流式解码器、采样缓存、HTTP 路由、模拟源子进程、进程退出清理。
 */
import http from 'node:http';
import {readFileSync, existsSync} from 'node:fs';
import {spawn} from 'node:child_process';
import {fileURLToPath, pathToFileURL} from 'node:url';
import path from 'node:path';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const page=readFileSync(path.join(root,'firmware/esp32s2/main/web/index.html'));
/*
 * 作用：独立计算线路数据的 CRC-16/CCITT-FALSE，用于交叉核对 C 编码器。
 * 参数：bytes 为 Buffer 或其他可遍历的 8 位字节序列。
 * 返回：无符号 16 位校验值。
 * 调用方式：crc = crc16(bytes)，由 Decoder.feed() 校验完整帧时调用。
 */
export function crc16(bytes){
 let crc=0xffff;for(const octet of bytes){crc^=octet<<8;for(let bit=0;bit<8;bit++)crc=((crc&0x8000)?(crc<<1)^0x1021:crc<<1)&0xffff;}return crc;
}
export class Decoder{
 /*
  * 作用：建立空接收缓存，并清零校验和格式错误计数。
  * 参数：无。
  * 返回：新建的 Decoder 实例，由构造过程返回。
  * 调用方式：decoder = new Decoder()，每条独立数据流使用自己的实例。
  */
 constructor(){this.buffer=Buffer.alloc(0);this.crcErrors=0;this.formatErrors=0;}
 /*
  * 作用：拼接输入数据，查找帧头、校验 CRC 并解码遥测帧。
  * 参数：chunk 为本次收到的 Buffer，可包含半帧或多个连续帧。
  * 返回：本次解析出的遥测对象数组；未完成帧保留在内部缓存。
  * 调用方式：frames = decoder.feed(chunk)，通常在子进程标准输出收到数据时调用。
  */
 feed(chunk){
  this.buffer=Buffer.concat([this.buffer,chunk]);const frames=[];
  while(this.buffer.length>=2){
   if(this.buffer[0]!==0xa5||this.buffer[1]!==0x5a){this.buffer=this.buffer.subarray(1);continue;}
   if(this.buffer.length<8)break;
   const length=this.buffer.readUInt16LE(6),total=length+10;
   if(this.buffer[2]!==1||length>48){this.formatErrors++;this.buffer=this.buffer.subarray(1);continue;}
   if(this.buffer.length<total)break;
   if(crc16(this.buffer.subarray(2,total-2))!==this.buffer.readUInt16LE(total-2)){this.crcErrors++;this.buffer=this.buffer.subarray(1);continue;}
   const frame=this.buffer.subarray(0,total);this.buffer=this.buffer.subarray(total);
   if(frame[3]!==1||length!==22||frame.readUInt16LE(14)>10000){this.formatErrors++;continue;}
   frames.push({sequence:frame.readUInt16LE(4),uptime_ms:frame.readUInt32LE(8),temperature:frame.readInt16LE(12)/100,humidity:frame.readUInt16LE(14)/100,can_tx:frame.readUInt32LE(16),can_rx:frame.readUInt32LE(20),can_errors:frame.readUInt32LE(24),flags:frame.readUInt16LE(28)});
  }
  return frames;
 }
}

/*
 * 作用：创建独立的模拟服务、采样缓存和故障控制状态，供命令行或测试复用。
 * 参数：可选配置中 now 为返回毫秒时间的函数，timeout 为数据过期毫秒数，默认 3500。
 * 返回：包含 server、ingest、snapshot 和 decoder 的对象；服务尚未开始监听。
 * 调用方式：demo = createDemoServer()，再调用 demo.server.listen() 和 demo.ingest(chunk)。
 */
export function createDemoServer({
 /*
  * 作用：提供默认的单调毫秒时钟，避免系统日期调整影响过期判断。
  * 参数：无。
  * 返回：当前单调时间，单位为毫秒。
  * 调用方式：未传入自定义 now 时，采样接收和状态快照内部通过 now() 调用。
  */
 now=()=>performance.now(),timeout=3500}={}){
 const decoder=new Decoder();let latest=null,lastAt=0,received=0,paused=false,canFault=false,errors=0,passes=0,attempts=0;
 /*
  * 作用：接收二进制数据流，应用暂停或故障状态，并更新最新采样及计数。
  * 参数：chunk 为模拟源输出的 Buffer 数据块。
  * 返回：无。
  * 调用方式：demo.ingest(chunk)，也可注册为子进程标准输出的 data 事件处理函数。
  */
 const ingest=chunk=>{
  for(const frame of decoder.feed(chunk)){
   if(paused)continue;
   if(frame.flags&8){
    ++attempts;if(canFault)++errors;else ++passes;
    latest={...frame,can_tx:attempts,can_rx:passes,can_errors:errors};
    if(canFault)latest.flags&=~1;
   }else latest={...frame};
   lastAt=now();received++;
  }
 };
 /*
  * 作用：依据当前时钟和最新采样生成网页状态，隐藏过期或无效测量值。
  * 参数：无；读取创建服务时捕获的时钟、超时配置和内部状态。
  * 返回：可序列化为 JSON 的状态对象。
  * 调用方式：state = demo.snapshot()，HTTP 状态路由和测试均可调用。
  */
 const snapshot=()=>{
  const age=latest?Math.max(0,Math.floor(now()-lastAt)):0,online=Boolean(latest&&age<timeout),valid=Boolean(online&&(latest.flags&1));
  const flags=latest?.flags??0,source=(flags&32)?'c2000-sensor':(flags&16)?'esp-local-simulation':(flags&8)?'host-simulation':(flags&4)?'c2000-loopback':'unknown';
  return {source,simulated:Boolean(flags&2),online,valid,sensor_online:valid&&Boolean(flags&32),bus_off:online&&Boolean(flags&64),temperature:valid?latest.temperature:null,humidity:valid?latest.humidity:null,age_ms:age,sequence:latest?.sequence??0,uptime_ms:latest?.uptime_ms??0,can_tx:latest?.can_tx??0,can_rx:latest?.can_rx??0,can_errors:latest?.can_errors??0,received_frames:received,crc_errors:decoder.crcErrors,format_errors:decoder.formatErrors,sim_paused:paused,sim_fault:canFault};
 };
 /*
  * 作用：处理首页、状态读取和模拟控制请求，并拒绝跨来源控制请求。
  * 参数：req 为 Node.js HTTP 请求流；res 为 HTTP 响应对象。
  * 返回：异步回调完成状态；实际结果写入 res，不向调用方返回业务数据。
  * 调用方式：注册到 http.createServer()，由服务器在收到请求时自动调用。
  */
 const server=http.createServer(async(req,res)=>{
  res.setHeader('Cache-Control','no-store');res.setHeader('X-Content-Type-Options','nosniff');
  if(req.method==='GET'&&req.url==='/'){res.writeHead(200,{'Content-Type':'text/html; charset=utf-8'});res.end(page);return;}
  if(req.method==='GET'&&req.url==='/api/status'){res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(snapshot()));return;}
  if(req.method==='POST'&&req.url==='/api/sim/control'){
   /* 服务仅监听本机，并拒绝浏览器从其他来源发起状态修改。 */
   if(req.headers.origin&&req.headers.origin!==`http://${req.headers.host}`){res.writeHead(403);res.end();return;}
   try{let body='';for await(const chunk of req){body+=chunk;if(body.length>1024)throw Error('oversize');}const input=JSON.parse(body);if(typeof input.paused==='boolean')paused=input.paused;if(typeof input.can_fault==='boolean')canFault=input.can_fault;res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify({paused,can_fault:canFault}));}catch{res.writeHead(400);res.end('Invalid control');}return;
  }
  res.writeHead(404);res.end('Not found');
 });
 return {server,ingest,snapshot,decoder};
}

if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href){
 const exe=path.join(root,'build/host/host_source.exe');
 if(!existsSync(exe)){console.error('Build first: powershell -File tools/build_host.ps1');process.exit(1);}
 const port=Number(process.env.PORT||8080),demo=createDemoServer();
 const producer=spawn(exe,[],{stdio:['ignore','pipe','inherit'],windowsHide:true});
 producer.stdout.on('data',demo.ingest);
 /*
  * 作用：报告模拟源启动错误，关闭服务并设置失败退出码。
  * 参数：error 为子进程发出的错误对象。
  * 返回：无。
  * 调用方式：注册为 producer 的 error 事件回调，由 Node.js 自动调用。
  */
 producer.on('error',error=>{console.error(error.message);demo.server.close();process.exitCode=1;});
 /*
  * 作用：记录模拟源的非零退出码。
  * 参数：code 为子进程退出码；因信号退出时可能为 null。
  * 返回：无。
  * 调用方式：注册为 producer 的 exit 事件回调，由 Node.js 自动调用。
  */
 producer.on('exit',code=>{if(code)console.error(`Simulation producer exited: ${code}`);});
 /*
  * 作用：报告服务器错误，停止模拟源并设置失败退出码。
  * 参数：error 为 HTTP 服务器发出的错误对象。
  * 返回：无。
  * 调用方式：注册为 demo.server 的 error 事件回调，由 Node.js 自动调用。
  */
 demo.server.on('error',error=>{console.error(error.message);producer.kill();process.exitCode=1;});
 /*
  * 作用：在本机端口监听成功后打印演示访问地址。
  * 参数：无。
  * 返回：无。
  * 调用方式：作为 listen() 的完成回调，由服务器监听成功后调用。
  */
 demo.server.listen(port,'127.0.0.1',()=>console.log(`PC SIMULATION (no hardware CAN/UART): http://127.0.0.1:${port}`));
 /*
  * 作用：终止模拟源子进程并关闭 HTTP 服务。
  * 参数：无。
  * 返回：无。
  * 调用方式：注册到 SIGINT 和 SIGTERM；用户按 Ctrl+C 或进程收到终止信号时调用。
  */
 const stop=()=>{producer.kill();demo.server.close();};process.on('SIGINT',stop);process.on('SIGTERM',stop);
}
