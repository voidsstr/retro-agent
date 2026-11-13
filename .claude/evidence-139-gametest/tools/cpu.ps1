param($n)
$p=Get-Process $n -ErrorAction Stop; $a=$p.TotalProcessorTime.TotalSeconds; Start-Sleep 5; $p.Refresh()
"cpu_s_in_5s=" + [math]::Round($p.TotalProcessorTime.TotalSeconds-$a,2) + " threads=" + $p.Threads.Count + " responding=" + $p.Responding
$p.Threads | Sort-Object TotalProcessorTime -Descending | Select-Object -First 4 | % { "tid {0} state {1} wait {2} cpu {3}" -f $_.Id,$_.ThreadState,$_.WaitReason,[math]::Round($_.TotalProcessorTime.TotalSeconds,1) }
