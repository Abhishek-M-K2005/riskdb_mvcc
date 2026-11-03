import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { exec } from 'child_process';
import path from 'path';
import fs from 'fs';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

function benchmarkRunnerPlugin() {
  const handler = (req, res) => {
    if (req.method === 'POST') {
      const projectRoot = path.resolve(__dirname, '..');
      const benchBin = path.join(projectRoot, 'build', 'mvcc_bench');
      const engineBin = path.join(projectRoot, 'build', 'mvcc_engine');

      // First run mvcc_engine to update telemetry
      exec(`${engineBin}`, { cwd: projectRoot }, () => {
        // Then run mvcc_bench to update latency curves
        exec(`${benchBin} --benchmark_format=json`, { cwd: projectRoot, maxBuffer: 10 * 1024 * 1024 }, (err, stdout, stderr) => {
          if (err) {
            res.statusCode = 500;
            res.setHeader('Content-Type', 'application/json');
            res.end(JSON.stringify({ error: err.message, stderr }));
            return;
          }
          try {
            const parsed = JSON.parse(stdout);
            const publicJson = path.join(__dirname, 'public', 'benchmark_results.json');
            fs.writeFileSync(publicJson, stdout, 'utf-8');
            const distDir = path.join(__dirname, 'dist');
            if (fs.existsSync(distDir)) {
              fs.writeFileSync(path.join(distDir, 'benchmark_results.json'), stdout, 'utf-8');
            }

            // Read engine telemetry if generated
            let telemetry = null;
            const telePath = path.join(__dirname, 'public', 'engine_telemetry.json');
            if (fs.existsSync(telePath)) {
              telemetry = JSON.parse(fs.readFileSync(telePath, 'utf-8'));
              if (fs.existsSync(distDir)) {
                fs.writeFileSync(path.join(distDir, 'engine_telemetry.json'), JSON.stringify(telemetry, null, 2), 'utf-8');
              }
            }

            res.setHeader('Content-Type', 'application/json');
            res.end(JSON.stringify({ success: true, data: parsed, telemetry }));
          } catch (e) {
            res.statusCode = 500;
            res.setHeader('Content-Type', 'application/json');
            res.end(JSON.stringify({ error: 'Failed to parse benchmark JSON output' }));
          }
        });
      });
    } else {
      res.statusCode = 405;
      res.end('Method Not Allowed');
    }
  };

  return {
    name: 'benchmark-runner',
    configureServer(server) {
      server.middlewares.use('/api/run-benchmark', handler);
    },
    configurePreviewServer(server) {
      server.middlewares.use('/api/run-benchmark', handler);
    }
  };
}

export default defineConfig({
  plugins: [react(), benchmarkRunnerPlugin()],
  server: {
    port: 3000
  },
  preview: {
    port: 3000
  }
});
