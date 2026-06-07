# Svelte + NetLeaf

Svelte 与 NetLeaf 后端集成示例。

## 快速开始

```bash
cd frontend-examples/svelte
npm install
npm run dev
```

## 示例代码

```svelte
<script>
  let message = '';

  async function fetchData() {
    const res = await fetch('http://localhost:8080/api');
    const data = await res.json();
    message = data.message;
  }

  fetchData();
</script>

<div>{message}</div>
```

## WebSocket 示例

```svelte
<script>
  let messages = [];
  let ws;

  function connect() {
    ws = new WebSocket('ws://localhost:8080/ws');
    ws.onmessage = (e) => {
      messages = [...messages, e.data];
    };
  }

  connect();
</script>

<ul>
  {#each messages as msg, i}
    <li key={i}>{msg}</li>
  {/each}
</ul>
```
