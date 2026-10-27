/*
 * 文件作用：交叉验证 C 编码器与 JavaScript 解码器，并检查电脑网页接口的状态变化。
 * 包含模块：原生模拟帧生成、CRC 与分段解析断言、HTTP 请求、可控时钟、故障恢复和资源清理。
 */
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {crc16,Decoder,createDemoServer} from '../tools/simulator.mjs';
const executable=fileURLToPath(new URL('../build/host/host_source.exe',import.meta.url));
const wire=execFileSync(executable,['--once','--step','42']);
const sensorExecutable=fileURLToPath(new URL('../build/host/sensor_fixture.exe',import.meta.url));

/*
 * 作用：验证协议页样例经生产 C 解码、UART 编码和网页状态接口后仍保持正确单位与来源。
 * 参数：无。
 * 返回：无；断言失败时由测试框架报告失败。
 * 调用方式：由 node --test 自动调用，不连接真实传感器或板卡。
 */
test('sensor frame to UART to web status, stale sensor differs from disconnected UART',()=>{
 const realWire=execFileSync(sensorExecutable,[]);
 const decoder=new Decoder(),[sample]=decoder.feed(realWire);
 assert.equal(sample.temperature,21.7);assert.equal(sample.humidity,54);assert.equal(sample.flags,33);
 const demo=createDemoServer();demo.ingest(realWire);
 let state=demo.snapshot();
 assert.equal(state.source,'c2000-sensor');assert.equal(state.simulated,false);
 assert.equal(state.sensor_online,true);assert.equal(state.can_tx,0);assert.equal(state.can_rx,1);
 demo.ingest(execFileSync(sensorExecutable,['--stale']));state=demo.snapshot();
 assert.equal(state.online,true);assert.equal(state.sensor_online,false);
 assert.equal(state.valid,false);assert.equal(state.temperature,null);
 demo.ingest(execFileSync(sensorExecutable,['--bus-off']));state=demo.snapshot();
 assert.equal(state.bus_off,true);assert.equal(state.valid,false);
 demo.ingest(realWire);state=demo.snapshot();assert.equal(state.sensor_online,true);
});

/*
 * 作用：核对原生 C 编码结果、独立 CRC、分段帧接收以及损坏帧后的重新同步。
 * 参数：无；使用文件启动时生成的 wire 测试帧。
 * 返回：无；断言失败时由测试框架报告失败。
 * 调用方式：执行 node --test tests/test_simulator.mjs，由测试框架自动调用该回调。
 */
test('native C encoder and independent JS decoder agree',()=>{
 assert.equal(wire.length,32);assert.equal(crc16(Buffer.from('123456789')),0x29b1);
 const decoder=new Decoder();assert.deepEqual(decoder.feed(wire.subarray(0,11)),[]);
 const [frame]=decoder.feed(wire.subarray(11));
 assert.equal(frame.temperature,26.2);assert.equal(frame.humidity,55.5);assert.equal(frame.sequence,42);
 assert.equal(frame.flags,11);assert.equal(frame.uptime_ms,42000);
 const bad=Buffer.from(wire);bad[12]^=1;
 assert.equal(decoder.feed(Buffer.concat([bad,wire,wire])).length,2);assert.equal(decoder.crcErrors,1);
});
/*
 * 作用：验证网页接口、数据过期、暂停、故障、恢复和错误请求响应。
 * 参数：t 为 Node.js 测试上下文，用于注册测试结束后的服务器清理。
 * 返回：异步测试完成状态；断言失败时由测试框架报告失败。
 * 调用方式：执行 node --test tests/test_simulator.mjs，由测试框架自动调用该回调。
 */
test('web endpoint, stale values, pause, fault and recovery',async t=>{
 let clock=10000;const demo=createDemoServer({
  /*
   * 作用：提供可由测试主动推进的毫秒时钟。
   * 参数：无。
   * 返回：当前测试变量 clock 的数值，单位为毫秒。
   * 调用方式：注入 createDemoServer() 的 now 配置，由模拟服务内部调用。
   */
  now:()=>clock});
 /*
  * 作用：在本机临时端口启动测试服务，等待监听完成。
  * 参数：resolve 为 Promise 的完成函数，传给 listen() 作为监听成功回调。
  * 返回：无；通过 resolve() 通知测试继续运行。
  * 调用方式：作为 new Promise() 的执行函数自动调用。
  */
 await new Promise(resolve=>demo.server.listen(0,'127.0.0.1',resolve));
 /*
  * 作用：测试结束时异步关闭服务器，避免监听句柄残留。
  * 参数：无。
  * 返回：服务器关闭后完成的 Promise。
  * 调用方式：通过 t.after() 注册，由测试框架在本测试结束后调用。
  */
 t.after(()=>new Promise(
  /*
   * 作用：将服务器关闭回调转换为 Promise 完成通知。
   * 参数：resolve 为 Promise 完成函数，作为 close() 的完成回调使用。
   * 返回：无。
   * 调用方式：由清理回调中的 new Promise() 自动调用。
   */
  resolve=>demo.server.close(resolve)));
 const base=`http://127.0.0.1:${demo.server.address().port}`;
 /*
  * 作用：请求模拟服务器的当前状态并解析 JSON。
  * 参数：无；请求地址使用当前测试服务的 base。
  * 返回：解析为状态对象的 Promise。
  * 调用方式：state = await status()，用于测试各阶段的状态断言。
  */
 const status=async()=>await(await fetch(base+'/api/status')).json();
 /*
  * 作用：提交模拟控制命令，并断言服务器返回成功状态码。
  * 参数：body 为控制对象，可包含 paused 和 can_fault 布尔值。
  * 返回：请求和状态码断言完成后的 Promise。
  * 调用方式：await control({paused:true}) 或 await control({can_fault:false})。
  */
 const control=async body=>{const r=await fetch(base+'/api/sim/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});assert.equal(r.status,200);};
 assert.equal((await status()).online,false);
 const html=await(await fetch(base)).text();assert.match(html,/温湿度为模拟数据/);assert.ok(!/https?:\/\//.test(html));
 demo.ingest(wire);assert.equal((await status()).temperature,26.2);
 await control({paused:true});clock+=4000;demo.ingest(wire);
 let s=await status();assert.equal(s.online,false);assert.equal(s.temperature,null);assert.equal(s.received_frames,1);
 await control({paused:false,can_fault:true});demo.ingest(wire);
 s=await status();assert.equal(s.online,true);assert.equal(s.valid,false);assert.equal(s.can_errors,1);assert.equal(s.can_rx,1);
 await control({can_fault:false});demo.ingest(wire);
 s=await status();assert.equal(s.valid,true);assert.equal(s.can_rx,2);assert.equal(s.source,'host-simulation');
 const bad=Buffer.from(wire);bad[15]^=1;demo.ingest(bad);assert.equal((await status()).crc_errors,1);
 assert.equal((await fetch(base+'/missing')).status,404);
 assert.equal((await fetch(base+'/api/sim/control',{method:'POST',body:'oops'})).status,400);
 assert.equal((await fetch(base+'/api/sim/control',{method:'POST',headers:{Origin:'https://example.com'},body:'{}'})).status,403);
});
