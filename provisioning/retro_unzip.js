// retro_unzip.js - extract a .zip into a folder on Win98/2K/XP with no unzip tool.
// Uses Shell.Application (CopyHere is ASYNC) and busy-waits for the destination
// item count to stabilize, so the caller does not delete the staging zip or
// proceed while the extract is still in flight. Same shim used by push-q3-mp-paks.py.
//   cscript //nologo retro_unzip.js <zip> <destFolder>
var sh = new ActiveXObject('Shell.Application');
var src = sh.NameSpace(WScript.Arguments(0));
var dst = sh.NameSpace(WScript.Arguments(1));
if (!src || !dst) { WScript.Echo('src/dst missing'); WScript.Quit(1); }
var wanted = src.Items().Count;
WScript.Echo('extracting ' + wanted + ' entries');
dst.CopyHere(src.Items(), 4 | 16);
var stable = 0, last = -1, poll = 0;
while (poll < 1800) {
  var cur = dst.Items().Count;
  if (cur >= wanted) { WScript.Echo('done ' + cur); break; }
  if (cur == last) { stable++; } else { stable = 0; last = cur; }
  if (stable > 25) { WScript.Echo('stalled at ' + cur + '/' + wanted); break; }
  WScript.Sleep(200); poll++;
}
