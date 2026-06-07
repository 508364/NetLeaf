# Vue + NetLeaf

Vue 3 与 NetLeaf 后端集成示例。

## 快速开始

```bash
cd frontend-examples/vue
npm install
npm run dev
```

## 组合式 API 示例

```vue
<script setup>
import { ref, onMounted } from 'vue';

const message = ref('');

onMounted(async () => {
  const res = await fetch('http://localhost:8080/api');
  const data = await res.json();
  message.value = data.message;
});
</script>

<template>
  <div>{{ message }}</div>
</template>
```

## WebSocket 示例

```vue
<script setup>
import { ref, onMounted, onUnmounted } from 'vue';

const messages = ref([]);
let ws;

onMounted(() => {
  ws = new WebSocket('ws://localhost:8080/ws');
  ws.onmessage = (e) => {
    messages.value.push(e.data);
  };
});

onUnmounted(() => ws.close());
</script>

<template>
  <ul>
    <li v-for="(msg, i) in messages" :key="i">{{ msg }}</li>
  </ul>
</template>
```
