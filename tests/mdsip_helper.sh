# mdsip replaces the old bespoke evaluator: GetManyExecute($) is MDSplus's own
# server-side batch evaluation. MDS_PATH must include tdi/remote or the server
# cannot find GetManyExecute.fun and fails with %TDI-E-UNKNOWN_VAR.
start_mdsip() {
  local port="$1" trees="$2" work="$3"
  printf '* | MAP_TO_LOCAL\n' > "$work/mdsip.hosts"
  MDS_PATH="$CONDA_PREFIX/tdi;$CONDA_PREFIX/tdi/remote" efit01_path="$trees" \
    mdsip -m -p "$port" -h "$work/mdsip.hosts" > "$work/mdsip.log" 2>&1 &
  MDSIP_PID=$!
  disown "$MDSIP_PID" 2>/dev/null || true
  for _ in $(seq 1 150); do
    kill -0 "$MDSIP_PID" 2>/dev/null || { echo "FAIL: mdsip exited"; cat "$work/mdsip.log"; exit 1; }
    (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null && { exec 3<&- 3>&-; return 0; }
    sleep 0.1
  done
  echo "FAIL: mdsip never listened on $port"; cat "$work/mdsip.log"; exit 1
}
