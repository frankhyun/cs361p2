PORT=9998

./server questions.txt $PORT &
SERVER_PID=$!
sleep 0.3

# test 1: first two correct and last NOANS (score should be 2)
RESULT=$(printf "Frank\r\nA\r\nC\r\n" | timeout 5 ./client localhost $PORT 2>/dev/null)
echo "$RESULT" | grep -q "Score: 2" && echo "PASS: all correct" || echo "FAIL: all correct"

kill $SERVER_PID 2>/dev/null; sleep 0.2

./server questions.txt $PORT &
SERVER_PID=$!
sleep 0.3