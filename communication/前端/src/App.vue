<template>
  <!-- 启动闪屏：第三张图片 -->
  <div v-if="stage === 'splash'" class="splash-screen">
    <img src="/a5d680e2e7ed822a93a4dd528b940eaa.jpg" class="splash-img" />
  </div>

  <!-- 登录页 -->
  <Login v-else-if="stage === 'login'" @login="onLogin" />

  <!-- 主界面 -->
  <Dashboard v-else :robot-port="robotPort" />
</template>

<script setup>
import { ref } from 'vue'
import Login from '@/views/Login.vue'
import Dashboard from '@/views/Dashboard.vue'

const stage = ref('splash')  // splash → login → transition → dashboard
const robotPort = ref('8765')

// 检查 localStorage 是否已登录
const saved = localStorage.getItem('robot_auth')
if (saved) {
  try {
    const data = JSON.parse(saved)
    if (data.port) {
      robotPort.value = data.port
      stage.value = 'dashboard'  // 已登录直接进主界面
    }
  } catch { /* ignore */ }
}

// 启动闪屏：显示第三张图片 1 秒后进入登录
if (stage.value === 'splash') {
  setTimeout(() => { stage.value = 'login' }, 1000)
}

function onLogin({ port }) {
  robotPort.value = port
  stage.value = 'dashboard'
  localStorage.setItem('robot_auth', JSON.stringify({ port }))
}
</script>

<style>
.splash-screen {
  width: 100vw; height: 100vh;
  display: flex; align-items: center; justify-content: center;
  background: #000;
}
.splash-img {
  width: 100vw;
  height: 100vh;
  object-fit: cover;
  animation: fadeIn 0.3s ease;
}
@keyframes fadeIn {
  from { opacity: 0; transform: scale(0.95); }
  to { opacity: 1; transform: scale(1); }
}
</style>
