/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{vue,js,ts,jsx,tsx}'],
  darkMode: 'class',
  theme: {
    extend: {
      colors: {
        apple: {
          blue: '#007AFF',
          'blue-hover': '#0062CC',
          green: '#34C759',
          yellow: '#FFCC02',
          orange: '#FF9500',
          red: '#FF3B30',
          gray: '#8E8E93',
        },
      },
      backdropBlur: {
        xs: '2px',
        glass: '20px',
        'glass-heavy': '40px',
      },
      borderRadius: {
        mac: '10px',
        'mac-lg': '14px',
        'mac-xl': '20px',
      },
      boxShadow: {
        mac: '0 8px 32px rgba(0, 0, 0, 0.12), 0 2px 8px rgba(0, 0, 0, 0.06)',
        'mac-dark': '0 8px 32px rgba(0, 0, 0, 0.4), 0 2px 8px rgba(0, 0, 0, 0.2)',
        'mac-window': '0 25px 50px -12px rgba(0, 0, 0, 0.25)',
      },
    },
  },
  plugins: [],
  corePlugins: {
    preflight: false, // 避免与Element Plus冲突
  },
}
