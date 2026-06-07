# React + NetLeaf

这是一个与 NetLeaf 后端配合使用的 React 示例应用。

## 快速开始

```bash
cd frontend-examples/react
npm install
npm start
```

同时启动 NetLeaf 后端：

```bash
cd ../..
./build_x64/bin/Release/simple_server.exe
```

访问 http://localhost:3000

## API 调用示例

```javascript
import { useEffect, useState } from 'react';

function App() {
  const [message, setMessage] = useState('');

  useEffect(() => {
    fetch('http://localhost:8080/api')
      .then(res => res.json())
      .then(data => setMessage(data.message));
  }, []);

  return <div>{message}</div>;
}
```

## WebSocket 示例

```javascript
import { useEffect, useRef, useState } from 'react';

function WsComponent() {
  const ws = useRef(null);
  const [messages, setMessages] = useState([]);

  useEffect(() => {
    ws.current = new WebSocket('ws://localhost:8080/ws');
    ws.current.onmessage = (e) => {
      setMessages(prev => [...prev, e.data]);
    };
    return () => ws.current.close();
  }, []);

  return (
    <ul>
      {messages.map((msg, i) => <li key={i}>{msg}</li>)}
    </ul>
  );
}
```
