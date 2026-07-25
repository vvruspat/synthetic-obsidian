import { useEffect, useRef } from "react";

function createShader(gl: WebGLRenderingContext, type: number, source: string) {
  const shader = gl.createShader(type);
  if (!shader) return null;
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    gl.deleteShader(shader);
    return null;
  }
  return shader;
}

function activateProgram(gl: WebGLRenderingContext, program: WebGLProgram) {
  // biome-ignore lint/correctness/useHookAtTopLevel: useProgram is a WebGL API method, not a React hook.
  gl.useProgram(program);
}

export function WebGLSurface() {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const gl = canvas.getContext("webgl", {
      alpha: true,
      antialias: false,
      premultipliedAlpha: false,
    });
    if (!gl) return;

    const vertex = createShader(
      gl,
      gl.VERTEX_SHADER,
      `attribute vec2 position;
       void main(){ gl_Position = vec4(position, 0.0, 1.0); }`,
    );
    const fragment = createShader(
      gl,
      gl.FRAGMENT_SHADER,
      `precision highp float;
       uniform vec2 resolution;
       float hash(float n){ return fract(sin(n) * 43758.5453123); }
       float noise(float x){
         float i = floor(x);
         float f = fract(x);
         f = f*f*(3.0-2.0*f);
         return mix(hash(i), hash(i+1.0), f);
       }
       void main(){
         vec2 p = gl_FragCoord.xy / resolution.xy;
         float top = 1.0 - p.y;
         float aspect = resolution.x / max(resolution.y, 1.0);
         vec3 col = vec3(.002, .012, .03);
         float hgrid = min(fract(top*20.0), 1.0-fract(top*20.0));
         col += vec3(.02,.12,.22) * smoothstep(.016,0.0,hgrid) * .27;
         if(top > .575){
           float py = (top-.575)/.425;
           col += vec3(.01,.04,.09) * noise(p.x*420.0 + py*29.0) * .38;
         }
         float vignette = smoothstep(.95,.2,length((p-.5)*vec2(1.0,aspect*.25)));
         col *= .72 + .28*vignette;
         gl_FragColor = vec4(col,1.0);
       }`,
    );
    if (!vertex || !fragment) return;
    const program = gl.createProgram();
    const buffer = gl.createBuffer();
    if (!program || !buffer) return;

    gl.attachShader(program, vertex);
    gl.attachShader(program, fragment);
    gl.linkProgram(program);
    activateProgram(gl, program);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]),
      gl.STATIC_DRAW,
    );
    const position = gl.getAttribLocation(program, "position");
    gl.enableVertexAttribArray(position);
    gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);
    const resolution = gl.getUniformLocation(program, "resolution");
    canvas.width = 1280;
    canvas.height = 720;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.uniform2f(resolution, canvas.width, canvas.height);
    gl.drawArrays(gl.TRIANGLES, 0, 6);

    return () => {
      gl.deleteBuffer(buffer);
      gl.deleteProgram(program);
      gl.deleteShader(vertex);
      gl.deleteShader(fragment);
    };
  }, []);

  return <canvas ref={canvasRef} className="webgl-surface" />;
}
