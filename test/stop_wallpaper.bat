@echo off
taskkill /IM webwallpaper64.exe /F
echo ---
timeout /t 2 /nobreak > NUL
nvidia-smi --query-gpu=utilization.gpu,memory.used,clocks.sm --format=csv,noheader
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader
