#include "shell.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <libgen.h>

/**
 * ====================================================================================
 * COMANDO: editor [archivo]   -- Categoria nueva: "editor"
 * ====================================================================================
 * DECISION ARQUITECTONICA (justificada en el PDF de sustentacion):
 *
 * Las categorias existentes ("datos", "memoria", "monitoreo", "utilidades")
 * agrupan DEMOSTRACIONES puntuales de una syscall: el handler ejecuta,
 * imprime la traza y retorna de inmediato al prompt "eafitOS>". El editor
 * "edi" es distinto por naturaleza: es una APLICACION INTERACTIVA COMPLETA
 * con su propio bucle de lectura de comandos que solo termina cuando el
 * usuario escribe 'q'. Forzarlo dentro de "datos" solo porque comparte
 * syscalls de archivos escondería esa diferencia de comportamiento.
 *
 * Por eso se crea la categoria "editor" con un unico comando registrado
 * ("editor"), cuyo handler NO reimplementa el editor: lo invoca como un
 * proceso hijo independiente via fork()+execve(), exactamente el mismo
 * patron ya usado por p_exec (categoria "monitoreo") para lanzar binarios
 * externos. Ventajas de este enfoque frente a compilar "edi" dentro del
 * mismo binario del shell:
 *   1. Aislamiento: un fallo del editor (segfault, buffer mal manejado)
 *      no derriba el shell educativo que lo invoco.
 *   2. El editor queda funcionando como programa standalone
 *      ("./editor/editor archivo.txt"), lo cual facilita el script de
 *      pruebas exigido en los entregables.
 *   3. Refleja fielmente como un shell real invoca a "vi" o "nano".
 *
 * La ruta del binario del editor se resuelve en tiempo de ejecucion con
 * readlink("/proc/self/exe", ...) para no depender del directorio desde el
 * cual se invoque "eafitOS" (cwd), solo de la ubicacion relativa fija entre
 * shell/ y editor/ dentro del repositorio (hermanos: ../editor/editor).
 */
int cmd_editor(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, COLOR_ERROR "Uso: editor [archivo]\n" COLOR_RESET);
        return 1;
    }

    /* 1. LLAMADA AL SISTEMA: readlink -- ubica el binario propio en disco */
    char self_path[PATH_MAX];
    LOG_SYSCALL("readlink", "\"/proc/self/exe\", self_path, %zu", sizeof(self_path) - 1);
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    self_path[len] = '\0';
    LOG_SYSCALL_RESULT(len);

    /* dirname() puede modificar su argumento; se usa una copia */
    char dir_copy[PATH_MAX];
    strncpy(dir_copy, self_path, sizeof(dir_copy) - 1);
    dir_copy[sizeof(dir_copy) - 1] = '\0';
    char *shell_dir = dirname(dir_copy); /* p.ej. ".../Editor-Parcial1/shell" */

    char editor_path[PATH_MAX];
    int written = snprintf(editor_path, sizeof(editor_path), "%s/../editor/editor", shell_dir);
    if (written < 0 || (size_t)written >= sizeof(editor_path)) {
        fprintf(stderr, COLOR_ERROR "Ruta del editor demasiado larga.\n" COLOR_RESET);
        return 1;
    }

    if (access(editor_path, X_OK) == -1) {
        fprintf(stderr, COLOR_ERROR "No se encontro el binario '%s'.\n" COLOR_RESET, editor_path);
        fprintf(stderr, COLOR_INFO "Compile el editor primero: (cd editor && make)\n" COLOR_RESET);
        return 1;
    }

    /* 2. LLAMADA AL SISTEMA: fork -- bifurca el shell para aislar al editor */
    LOG_SYSCALL("fork", "");
    /* fork() duplica el proceso TAL CUAL esta en RAM, incluido cualquier
     * texto que printf() ya haya puesto en el buffer de stdio pero que aun
     * no se haya volcado a pantalla (no hay '\n' en la traza de arriba).
     * Sin este fflush(), ese texto pendiente quedaria copiado en padre E
     * hijo, y ambos lo terminarian imprimiendo por separado (salida
     * duplicada) cuando cada uno vacie su propia copia del buffer. */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }

    if (pid == 0) {
        /* PROCESO HIJO: reemplaza su imagen de memoria por la del editor */
        char *exec_args[3];
        exec_args[0] = editor_path;
        exec_args[1] = (argc == 2) ? argv[1] : NULL;
        exec_args[2] = NULL;

        /* 3. LLAMADA AL SISTEMA: execve */
        LOG_SYSCALL("execve", "\"%s\", argv, envp", editor_path);
        printf("\n" COLOR_INFO "[Hijo] Cediendo control interactivo a 'edi'...\n\n" COLOR_RESET);
        /* CRITICO: execve(), si tiene exito, reemplaza toda la imagen del
         * proceso (incluido el buffer interno de stdio) sin ejecutar ningun
         * flush automatico. Cualquier printf() pendiente en el buffer se
         * PERDERIA en silencio si no se vacia explicitamente antes. */
        fflush(stdout);

        extern char **environ;
        execve(editor_path, exec_args, environ);

        /* Solo se llega aqui si execve() fallo */
        LOG_SYSCALL_ERROR(strerror(errno));
        fprintf(stderr, COLOR_ERROR "Error: no se pudo ejecutar el editor.\n" COLOR_RESET);
        exit(127);
    } else {
        /* PROCESO PADRE: espera a que el usuario termine la sesion de edicion ('q') */
        LOG_SYSCALL_RESULT(pid);
        printf(COLOR_PROMPT "[Shell]" COLOR_RESET " Editor lanzado (PID %d). Esperando a que finalice ('q' para volver)...\n", pid);

        int status;
        LOG_SYSCALL("waitpid", "%d, &status, 0", pid);
        pid_t waited_pid = waitpid(pid, &status, 0);
        if (waited_pid == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            return 1;
        }
        LOG_SYSCALL_RESULT(waited_pid);

        if (WIFEXITED(status)) {
            printf(COLOR_PROMPT "[Shell]" COLOR_RESET " Sesion de edicion finalizada. Codigo de salida: " COLOR_RESULT "%d" COLOR_RESET "\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf(COLOR_PROMPT "[Shell]" COLOR_RESET " El editor fue terminado por senal: " COLOR_ERROR "%d" COLOR_RESET "\n", WTERMSIG(status));
        }
    }
    return 0;
}
