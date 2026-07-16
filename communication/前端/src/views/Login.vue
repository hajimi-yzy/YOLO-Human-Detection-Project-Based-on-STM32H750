<template>
  <div class="login-page">
    <div class="login-card">
      <div class="text-center mb-6">
        <h1 class="text-xl font-bold" style="color: var(--text-primary)">机器人远程控制</h1>
        <p class="text-xs mt-1" style="color: var(--text-tertiary)">请输入连接信息以继续</p>
      </div>

      <form @submit.prevent="handleLogin" class="flex flex-col gap-4">
        <div class="flex flex-col gap-1">
          <label class="text-xs font-medium" style="color: var(--text-secondary)">机器人端口号</label>
          <input v-model="port" type="text" placeholder="例如: 8765" class="login-input" autocomplete="off" />
        </div>

        <div class="flex flex-col gap-1">
          <label class="text-xs font-medium" style="color: var(--text-secondary)">管理员密码</label>
          <input v-model="password" type="password" placeholder="请输入密码" class="login-input" autocomplete="off" />
        </div>

        <p v-if="errorMsg" class="text-xs text-red-500 text-center">{{ errorMsg }}</p>

        <button type="submit" class="login-btn" :disabled="loading">
          {{ loading ? '验证中...' : '登 录' }}
        </button>
      </form>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { login } from '@/api'

const emit = defineEmits(['login'])

const port = ref('8765')
const password = ref('')
const errorMsg = ref('')
const loading = ref(false)

async function handleLogin() {
  errorMsg.value = ''
  if (!port.value.trim()) { errorMsg.value = '请输入端口号'; return }
  if (!password.value) { errorMsg.value = '请输入管理员密码'; return }

  loading.value = true
  try {
    await login(password.value)
    emit('login', { port: port.value.trim() })
  } catch {
    errorMsg.value = '登录失败，请检查密码和服务器配置'
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.login-page {
  width: 100vw; height: 100vh;
  display: flex; align-items: center; justify-content: center;
  background: url('/934f22e3cb753cfb859ee0cd0b190ebe.jpg') center / cover no-repeat;
}
.login-card {
  width: 380px;
  padding: 36px 32px;
  border-radius: 16px;
  background: rgba(255,255,255,0.18);
  backdrop-filter: blur(24px) saturate(180%);
  -webkit-backdrop-filter: blur(24px) saturate(180%);
  border: 1px solid rgba(255,255,255,0.3);
  box-shadow: 0 20px 60px rgba(0,0,0,0.25);
}
.login-input {
  width: 100%;
  padding: 8px 12px;
  border-radius: 8px;
  border: 1px solid var(--border-color);
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
  transition: border-color 0.2s;
}
.login-input:focus { border-color: #007aff; }
.login-btn {
  width: 100%;
  padding: 10px;
  border-radius: 8px;
  border: none;
  background: #007aff;
  color: #fff;
  font-size: 14px;
  font-weight: 600;
  cursor: pointer;
  transition: opacity 0.2s;
}
.login-btn:hover { opacity: 0.85; }
.login-btn:disabled { opacity: 0.5; cursor: default; }
</style>
