// Exercise the real theme UI functions with a small DOM adapter, no browser dependency.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync('src/webui.html','utf8');
const code=html.slice(html.indexOf('function themeRequest('),html.indexOf('function upload(){'));
const jsonHelper=html.slice(html.indexOf('function j('),html.indexOf('// tabs'));
const nodes={};
for(const id of ['themeSelect','themeUseBtn','themeRemoveBtn','themeDetails','themeStatus','mode'])nodes[id]={value:'',textContent:'',disabled:false,children:[],appendChild(child){this.children.push(child)}};
let response,requests=[],toast='';
const context={
 $:id=>nodes[id],C:{},Promise,JSON,Math,FormData:class {},
 document:{createElement:()=>({})},j:async()=>response,
 fetch:async(path,options)=>{requests.push([path,JSON.parse(options.body)]);return {ok:true,json:async()=>({id:'broken'})}},
 toast:message=>toast=message,sv:(id,value)=>nodes[id].value=value,modeChanged:()=>{}
};
vm.createContext(context);vm.runInContext(code,context);
(async()=>{
 const helperContext={fetch:async()=>({ok:false,status:500,json:async()=>({error:'could not save settings'})})};
 vm.createContext(helperContext);vm.runInContext(jsonHelper,helperContext);
 await assert.rejects(helperContext.j('/api/config'),/could not save settings/);
 response={selected:'broken',freeBytes:65536,themes:[
  {id:'',name:'Built-in clock',builtin:true,valid:true,bytes:0},
  {id:'healthy',name:'Clock',version:'1',bytes:100,valid:true},
  {id:'broken',name:'broken',bytes:1234,valid:false,error:'Invalid or truncated package'}]};
 await context.loadThemes();
 assert.equal(nodes.themeSelect.children.length,3);
 assert.equal(nodes.themeSelect.children[0].textContent,'Built-in clock');
 assert.match(nodes.themeSelect.children[2].textContent,/invalid package/);
 assert.match(nodes.themeStatus.textContent,/^2 installed/);
 assert.equal(nodes.themeUseBtn.disabled,true);
 assert.match(nodes.themeDetails.textContent,/Invalid or truncated package/);
 context.selectTheme();assert.equal(requests.length,0);assert.match(toast,/valid installed theme/);
 context.deleteTheme();await new Promise(resolve=>setImmediate(resolve));
 assert.equal(requests[0][0],'/api/themes/delete');assert.equal(requests[0][1].id,'broken');
 nodes.themeSelect.value='healthy';context.themeChoiceChanged();assert.equal(nodes.themeUseBtn.disabled,false);
 assert.equal(nodes.themeRemoveBtn.disabled,false);
 response={selected:'',freeBytes:65536,themes:[]};await context.loadThemes();assert.equal(nodes.themeUseBtn.disabled,true);
 // The built-in theme is selectable but never removable, and a recovery is reported.
 response={selected:'',recovered:'big-theme',freeBytes:65536,themes:[{id:'',name:'Built-in clock',builtin:true,valid:true,bytes:0}]};
 await context.loadThemes();
 assert.equal(nodes.themeSelect.value,'');
 assert.equal(nodes.themeUseBtn.disabled,false);assert.equal(nodes.themeRemoveBtn.disabled,true);
 assert.match(nodes.themeDetails.textContent,/built in/);
 assert.match(nodes.themeStatus.textContent,/big-theme.*turned off · 0 installed/);
 requests=[];context.deleteTheme();assert.equal(requests.length,0);assert.match(toast,/cannot be removed/);
 context.selectTheme();await new Promise(resolve=>setImmediate(resolve));
 assert.equal(requests[0][0],'/api/themes/select');assert.equal(requests[0][1].id,'');
 console.log('theme web UI tests passed');
})().catch(error=>{console.error(error);process.exitCode=1});
