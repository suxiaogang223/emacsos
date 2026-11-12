;;; emacsos-app.el --- Trusted application instances -*- lexical-binding: t; -*-

;; Copyright (C) 2026 emacsOS contributors
;; SPDX-License-Identifier: GPL-3.0-or-later

;;; Commentary:

;; Cooperative application policy for one trusted Emacs runtime.  Registration
;; tracks explicit resources, not ambient Emacs activity or kernel ownership.
;; Entry return ends startup.  Applications explicitly finish or request stop.
;; Cleanup runs after every already-entered managed callback has unwound.

;;; Code:

(require 'cl-lib)
(require 'tabulated-list)

(define-error 'emacsos-app-error "Application management error")
(define-error 'emacsos-app-expired "Application history expired" 'emacsos-app-error)

(defconst emacsos-app-max-records 64 "Maximum live and terminal run records.")
(defconst emacsos-app-max-resources 64 "Maximum registrations per live run.")
(defconst emacsos-app-max-logs 32 "Maximum retained log entries per run.")
(defconst emacsos-app-max-cleanup-errors 32 "Maximum retained cleanup errors per run.")
(defconst emacsos-app-max-text 512 "Maximum characters per diagnostic field.")

(cl-defstruct (emacsos-app--run (:constructor emacsos-app--make-run))
  id name entry args source state active pending result error observed
  resources logs (lost-logs 0) cleanup-errors (lost-cleanup-errors 0) wait)
(cl-defstruct (emacsos-app--resource (:constructor emacsos-app--make-resource))
  id value release kind mode shared consumed)
(cl-defstruct (emacsos-app--shared (:constructor emacsos-app--make-shared))
  value release (refs 0))

(defvar emacsos-app--runs (make-hash-table :test #'eql))
(defvar emacsos-app--next-id 0)
(defvar emacsos-app--next-resource 0)
(defvar emacsos-app-current nil
  "Run identity during managed entry or callback, otherwise nil.
This dynamic context conveys ownership intent, not a security boundary.")

(defun emacsos-app--text (value)
  "Return a bounded diagnostic representation of VALUE."
  (let* ((print-length 12) (print-level 4)
         (text (if (stringp value) value (prin1-to-string value))))
    (substring text 0 (min (length text) emacsos-app-max-text))))

(defun emacsos-app--get (id)
  (or (gethash id emacsos-app--runs)
      (signal (if (and (integerp id) (> id 0) (<= id emacsos-app--next-id))
                  'emacsos-app-expired 'emacsos-app-error)
              (list id))))

(defun emacsos-app--terminal-p (run)
  (memq (emacsos-app--run-state run) '(exited failed)))

(defun emacsos-app--accepting-p (run)
  (and (eq run (gethash (emacsos-app--run-id run) emacsos-app--runs))
       (not (emacsos-app--run-pending run))
       (memq (emacsos-app--run-state run) '(starting running))))

(defun emacsos-app--acquire-check (run &optional count)
  (unless (emacsos-app--accepting-p run)
    (signal 'emacsos-app-error '("Run no longer accepts resources")))
  (when (> (+ (length (emacsos-app--run-resources run)) (or count 1))
           emacsos-app-max-resources)
    (signal 'emacsos-app-error '("Resource registration capacity exhausted"))))

(defun emacsos-app-log (id text)
  "Append bounded diagnostic TEXT for ID, evicting oldest entries explicitly."
  (let* ((run (emacsos-app--get id))
         (entries (cons (emacsos-app--text text) (emacsos-app--run-logs run))))
    (when (> (length entries) emacsos-app-max-logs)
      (setcdr (nthcdr (1- emacsos-app-max-logs) entries) nil)
      (cl-incf (emacsos-app--run-lost-logs run)))
    (setf (emacsos-app--run-logs run) entries)))

(defun emacsos-app--cleanup-diagnostic (run value)
  (let ((text (emacsos-app--text value)))
    (push text (emacsos-app--run-cleanup-errors run))
    (when (> (length (emacsos-app--run-cleanup-errors run)) emacsos-app-max-cleanup-errors)
      (setcdr (nthcdr (1- emacsos-app-max-cleanup-errors)
                     (emacsos-app--run-cleanup-errors run)) nil)
      (cl-incf (emacsos-app--run-lost-cleanup-errors run)))
    (emacsos-app-log (emacsos-app--run-id run) text)))

(defun emacsos-app--release (run resource)
  "Consume RESOURCE before calling its release rule."
  (unless (emacsos-app--resource-consumed resource)
    (setf (emacsos-app--resource-consumed resource) t)
    (let ((release (emacsos-app--resource-release resource))
          (value (emacsos-app--resource-value resource))
          (mode (emacsos-app--resource-mode resource)))
      (when (eq mode 'shared)
        (let ((shared (emacsos-app--resource-shared resource)))
          (cl-decf (emacsos-app--shared-refs shared))
          (setq release (and (= 0 (emacsos-app--shared-refs shared))
                             (emacsos-app--shared-release shared))
                value (emacsos-app--shared-value shared))))
      (when (and release (not (eq mode 'borrowed)))
        (let (completed)
          (unwind-protect
              (condition-case condition
                  (let ((result (funcall release value)))
                    (setq completed t)
                    (when (memq result '(preserved-modified preserved-buffer))
                      (emacsos-app-log
                       (emacsos-app--run-id run)
                       (format "Buffer returned to user ownership: %s" result))))
                ((error quit)
                 (setq completed t)
                 (emacsos-app--cleanup-diagnostic run condition)))
            (unless completed
              (emacsos-app--cleanup-diagnostic
               run '(nonlocal-exit "Release rule did not return")))))))))

(defun emacsos-app--release-all (run resources)
  ;; Unwind protection also attempts remaining releases after an arbitrary throw.
  (when resources
    (unwind-protect (emacsos-app--release run (car resources))
      (emacsos-app--release-all run (cdr resources)))))

(defun emacsos-app--complete (run)
  (when (and (emacsos-app--run-pending run)
             (= 0 (emacsos-app--run-active run))
             (not (memq (emacsos-app--run-state run) '(cleaning exited failed))))
    (setf (emacsos-app--run-state run) 'cleaning)
    (let ((resources (emacsos-app--run-resources run))
          (emacsos-app-current (emacsos-app--run-id run)))
      (setf (emacsos-app--run-resources run) nil)
      (unwind-protect (emacsos-app--release-all run resources)
        (setf (emacsos-app--run-state run)
              (if (or (eq (emacsos-app--run-pending run) 'failed)
                      (emacsos-app--run-cleanup-errors run)) 'failed 'exited)
              (emacsos-app--run-wait run) nil)
        (emacsos-app-log (emacsos-app--run-id run) "Cleanup barrier complete")))))

(defun emacsos-app--request (run reason &optional value)
  (unless (emacsos-app--terminal-p run)
    (when (or (not (emacsos-app--run-pending run))
              (and (eq reason 'failed) (not (emacsos-app--run-error run))))
      (setf (emacsos-app--run-pending run) reason
            (emacsos-app--run-state run)
            (if (eq (emacsos-app--run-state run) 'cleaning) 'cleaning
              (if (eq reason 'stopped) 'stop-requested 'completing)))
      (if (eq reason 'failed)
          (setf (emacsos-app--run-error run) (emacsos-app--text value))
        (setf (emacsos-app--run-result run) (emacsos-app--text value))))
    (emacsos-app--complete run)))

(defun emacsos-app--invoke (run function args)
  (when (emacsos-app--accepting-p run)
    (let ((emacsos-app-current (emacsos-app--run-id run)) completed)
      (cl-incf (emacsos-app--run-active run))
      (unwind-protect
          (condition-case condition
              (prog1 (apply function args) (setq completed t))
            ((error quit)
             (setq completed t)
             (emacsos-app--request run 'failed condition)
             (signal (car condition) (cdr condition))))
        (unless completed
          (emacsos-app--request run 'failed '(nonlocal-exit "Entry did not return")))
        (cl-decf (emacsos-app--run-active run))
        (emacsos-app--complete run)))))

(defun emacsos-app-start (name entry &optional args source)
  "Start trusted ENTRY with ARGS as a new run named NAME; return its identity.
SOURCE is an optional source file.  Returning from ENTRY ends startup only.
Errors and quits retain their original signal after recording failed cleanup.
An arbitrary nonlocal exit also unwinds bookkeeping and preserves its exit.
The run remains inspectable on failure even when this call does not return."
  (unless (and (stringp name) (functionp entry) (listp args)
               (or (null source) (stringp source)))
    (signal 'wrong-type-argument '("Invalid application entry")))
  (when (>= (hash-table-count emacsos-app--runs) emacsos-app-max-records)
    (let (oldest)
      (maphash (lambda (id run)
                 (when (and (emacsos-app--terminal-p run)
                            (or (not oldest) (< id oldest)))
                   (setq oldest id))) emacsos-app--runs)
      (unless oldest
        (signal 'emacsos-app-error '("Run capacity exhausted; live runs retained")))
      (remhash oldest emacsos-app--runs)))
  (let* ((id (cl-incf emacsos-app--next-id))
         (run (emacsos-app--make-run
               :id id :name (emacsos-app--text name) :entry entry :args args
               :source (and source (copy-sequence source)) :state 'starting :active 0)))
    (puthash id run emacsos-app--runs)
    (emacsos-app-log id "Starting trusted shared-runtime instance")
    (emacsos-app--invoke run entry args)
    (when (eq (emacsos-app--run-state run) 'starting)
      (setf (emacsos-app--run-state run) 'running))
    id))

(defun emacsos-app-request-stop (id)
  "Request cooperative stop of ID, without pumping or blocking the command loop.
Pending callbacks become ineligible immediately.  Already-entered callbacks
must unwind before resources are released.  This cannot stop hung Lisp."
  (emacsos-app--request (emacsos-app--get id) 'stopped)
  (emacsos-app-inspect id))

(defun emacsos-app-finish (id &optional result)
  "Declare ID finished with diagnostic RESULT after its callback barrier."
  (emacsos-app--request (emacsos-app--get id) 'finished result)
  (emacsos-app-inspect id))

(defun emacsos-app-set-wait-reason (id reason)
  "Set diagnostic wait REASON for live ID, or clear it with nil."
  (let ((run (emacsos-app--get id)))
    (emacsos-app--acquire-check run 0)
    (setf (emacsos-app--run-wait run) (and reason (emacsos-app--text reason)))))

(defun emacsos-app--snapshot (run)
  (list :id (emacsos-app--run-id run) :name (copy-sequence (emacsos-app--run-name run))
        :backend 'trusted-shared-emacs :state (emacsos-app--run-state run)
        :active (emacsos-app--run-active run) :reason (emacsos-app--run-pending run)
        :wait (copy-sequence (emacsos-app--run-wait run))
        :source (copy-sequence (emacsos-app--run-source run))
        :result (copy-sequence (emacsos-app--run-result run))
        :error (copy-sequence (emacsos-app--run-error run))
        :cleanup-errors (mapcar #'copy-sequence (reverse (emacsos-app--run-cleanup-errors run)))
        :lost-cleanup-errors (emacsos-app--run-lost-cleanup-errors run)
        :logs (mapcar #'copy-sequence (reverse (emacsos-app--run-logs run)))
        :lost-logs (emacsos-app--run-lost-logs run)
        :resources (mapcar
                    (lambda (resource)
                      (list :id (emacsos-app--resource-id resource)
                            :kind (emacsos-app--resource-kind resource)
                            :ownership (emacsos-app--resource-mode resource)))
                    (reverse (emacsos-app--run-resources run)))))

(defun emacsos-app-inspect (id)
  "Return detached state for ID, observing its outcome if terminal.
Expired history signals `emacsos-app-expired'; identities are never reused."
  (let ((run (emacsos-app--get id)))
    (when (emacsos-app--terminal-p run)
      (setf (emacsos-app--run-observed run) t))
    (emacsos-app--snapshot run)))

(defun emacsos-app-list ()
  "Return detached snapshots ordered by identity, without observing outcomes."
  (let (runs)
    (maphash (lambda (_ run) (push (emacsos-app--snapshot run) runs)) emacsos-app--runs)
    (sort runs (lambda (a b) (< (plist-get a :id) (plist-get b :id))))))

(defun emacsos-app-wait (id)
  "Poll ID once; return (:status pending|complete :run SNAPSHOT).
This MVP operation has no subscription, deadline or nested event-loop wait."
  (let* ((snapshot (emacsos-app-inspect id))
         (terminal (memq (plist-get snapshot :state) '(exited failed))))
    (list :status (if terminal 'complete 'pending) :run snapshot)))

(defun emacsos-app-restart (id)
  "Start a fresh invocation after ID has exited and its outcome was observed.
Use `emacsos-app-inspect' or `emacsos-app-wait' to observe the terminal run.
Never terminate or reuse the old run implicitly."
  (let ((run (emacsos-app--get id)))
    (unless (and (emacsos-app--terminal-p run) (emacsos-app--run-observed run))
      (signal 'emacsos-app-error '("Observe terminal outcome before restart")))
    (emacsos-app-start (emacsos-app--run-name run) (emacsos-app--run-entry run)
                       (emacsos-app--run-args run) (emacsos-app--run-source run))))

(defun emacsos-app-adopt (id value release &optional kind ownership)
  "Track VALUE for ID using RELEASE, KIND and OWNERSHIP (owned or borrowed).
Adoption is explicit.  Borrowing never releases VALUE.  An owning release rule
receives VALUE and must return or signal an ordinary condition.  It is called
once after consumption, even if it fails.  Return a registration identity."
  (let ((run (emacsos-app--get id)) (mode (or ownership 'owned)))
    (unless (and (memq mode '(owned borrowed)) (symbolp kind)
                 (or (eq mode 'borrowed) (functionp release)))
      (signal 'emacsos-app-error '("Invalid ownership or release rule")))
    (emacsos-app--acquire-check run)
    (when (eq mode 'owned)
      (maphash
       (lambda (_ other)
         (dolist (resource (emacsos-app--run-resources other))
           (when (and (eq value (emacsos-app--resource-value resource))
                      (not (eq (emacsos-app--resource-mode resource) 'borrowed)))
             (signal 'emacsos-app-error '("Resource already owned; share or transfer explicitly")))))
       emacsos-app--runs))
    (let ((resource (emacsos-app--make-resource
                     :id (cl-incf emacsos-app--next-resource)
                     :value value :release release :kind (or kind 'custom) :mode mode)))
      (push resource (emacsos-app--run-resources run))
      (emacsos-app--resource-id resource))))

(defun emacsos-app--resource (run registration)
  (or (cl-find registration (emacsos-app--run-resources run)
               :key #'emacsos-app--resource-id)
      (signal 'emacsos-app-error '("Unknown resource registration"))))

(defun emacsos-app-release (id registration)
  "Consume ID's REGISTRATION and apply its ownership release rule once."
  (let* ((run (emacsos-app--get id))
         (resource (emacsos-app--resource run registration)))
    (when (> (emacsos-app--run-active run) 0)
      (signal 'emacsos-app-error '("Release must follow the active callback barrier")))
    (setf (emacsos-app--run-resources run)
          (delq resource (emacsos-app--run-resources run)))
    (emacsos-app--release run resource)))

(defun emacsos-app-transfer (from registration to)
  "Atomically transfer REGISTRATION from FROM to accepting TO.
Failed validation leaves FROM responsible.  Return the unchanged registration."
  (let* ((source (emacsos-app--get from)) (destination (emacsos-app--get to))
         (resource (emacsos-app--resource source registration)))
    (emacsos-app--acquire-check source 0)
    (emacsos-app--acquire-check destination)
    (when (eq source destination)
      (signal 'emacsos-app-error '("Transfer destination must differ")))
    (when (> (emacsos-app--run-active source) 0)
      (signal 'emacsos-app-error '("Transfer must follow the active callback barrier")))
    (setf (emacsos-app--run-resources source)
          (delq resource (emacsos-app--run-resources source)))
    (push resource (emacsos-app--run-resources destination))
    registration))

(defun emacsos-app-share (from registration to)
  "Add a shared reference in TO for FROM's owned/shared REGISTRATION.
The original release runs once after the last explicit reference is released.
Return TO's new registration.  Borrowed registrations cannot be shared."
  (let* ((source (emacsos-app--get from)) (destination (emacsos-app--get to))
         (resource (emacsos-app--resource source registration)))
    (emacsos-app--acquire-check source 0)
    (emacsos-app--acquire-check destination)
    (unless (memq (emacsos-app--resource-mode resource) '(owned shared))
      (signal 'emacsos-app-error '("Borrowed resources cannot confer ownership")))
    (let* ((shared (or (emacsos-app--resource-shared resource)
                       (emacsos-app--make-shared
                        :value (emacsos-app--resource-value resource)
                        :release (emacsos-app--resource-release resource) :refs 1)))
           (copy (emacsos-app--make-resource
                  :id (cl-incf emacsos-app--next-resource)
                  :value (emacsos-app--resource-value resource)
                  :kind (emacsos-app--resource-kind resource) :mode 'shared :shared shared)))
      (setf (emacsos-app--resource-mode resource) 'shared
            (emacsos-app--resource-shared resource) shared)
      (cl-incf (emacsos-app--shared-refs shared))
      (push copy (emacsos-app--run-resources destination))
      (emacsos-app--resource-id copy))))

(defun emacsos-app--callback (id function)
  "Register FUNCTION for ID; return (CALLBACK . REGISTRATION)."
  (unless (functionp function)
    (signal 'wrong-type-argument (list 'functionp function)))
  (let ((run (emacsos-app--get id)) (enabled t))
    (let ((registration (emacsos-app-adopt
                         id (list 'callback function)
                         (lambda (_) (setq enabled nil)) 'callback)))
      (cons (lambda (&rest args)
              (when enabled (emacsos-app--invoke run function args)))
            registration))))

(defun emacsos-app-wrap (id function)
  "Register FUNCTION and return a callback capturing ID's run context.
Late delivery after finish/stop/history eviction is ignored.  Registering the
callback consumes one resource slot until explicit release or run cleanup."
  (car (emacsos-app--callback id function)))

(defun emacsos-app-run-at-time (id time repeat function &rest args)
  "Create an explicitly owned timer for ID, TIME, REPEAT, FUNCTION and ARGS.
Its callback captures ID and rejects late delivery after retirement."
  (emacsos-app--acquire-check (emacsos-app--get id) 2)
  (let* ((run (emacsos-app--get id))
         (callback (emacsos-app--callback id function)) timer completed)
    (unwind-protect
        (prog1
            (progn
              (setq timer (apply #'run-at-time time repeat (car callback) args))
              (emacsos-app-adopt id timer #'cancel-timer 'timer)
              timer)
          (setq completed t))
      (unless completed
        (when timer (cancel-timer timer))
        ;; This newly registered callback cannot have entered; rollback is safe
        ;; even inside the acquiring entry's active-callback barrier.
        (let ((resource (emacsos-app--resource run (cdr callback))))
          (setf (emacsos-app--run-resources run)
                (delq resource (emacsos-app--run-resources run)))
          (emacsos-app--release run resource))))))

(defun emacsos-app--release-buffer (buffer)
  (when (buffer-live-p buffer)
    (with-current-buffer buffer
      (if (buffer-modified-p) 'preserved-modified
        (let* ((preserve (make-symbol "preserve-modified-buffer"))
               (kill-buffer-query-functions
                (append kill-buffer-query-functions
                        (list (lambda () (not (buffer-modified-p buffer))))))
               (kill-buffer-hook
                (append kill-buffer-hook
                        (list (lambda ()
                                (when (buffer-modified-p buffer)
                                  (throw preserve 'preserved-modified)))))))
          ;; User hooks can edit an initially clean buffer.  Recheck after both
          ;; normal query functions and normal kill hooks before destroying it.
          (catch preserve
            (if (kill-buffer buffer) 'released 'preserved-buffer)))))))

(defun emacsos-app-adopt-buffer (id buffer &optional take-ownership)
  "Register existing BUFFER as borrowed unless TAKE-OWNERSHIP is explicit.
Owned buffers are returned to user ownership when modified or kill is vetoed."
  (unless (buffer-live-p buffer)
    (signal 'wrong-type-argument (list 'buffer-live-p buffer)))
  (emacsos-app-adopt id buffer #'emacsos-app--release-buffer 'buffer
                     (if take-ownership 'owned 'borrowed)))

(defun emacsos-app-buffer (id name)
  "Create a new owned buffer named from NAME for ID.
This never adopts a pre-existing user or file buffer implicitly."
  (emacsos-app--acquire-check (emacsos-app--get id))
  (let ((buffer (generate-new-buffer name)))
    (condition-case condition
        (progn (emacsos-app-adopt-buffer id buffer t) buffer)
      ((error quit) (kill-buffer buffer) (signal (car condition) (cdr condition))))))

(defvar emacsos-processes-mode-map
  (let ((map (make-sparse-keymap)))
    (set-keymap-parent map tabulated-list-mode-map)
    (define-key map (kbd "g") #'emacsos-processes-refresh)
    (define-key map (kbd "s") #'emacsos-processes-stop)
    (define-key map (kbd "r") #'emacsos-processes-restart)
    (define-key map (kbd "RET") #'emacsos-processes-inspect)
    (define-key map (kbd "v") #'emacsos-processes-source)
    map))

(define-derived-mode emacsos-processes-mode tabulated-list-mode "Processes"
  "Inspect trusted application instances; s stop, r restart, RET details, v source."
  (setq tabulated-list-format [("Run" 7 t) ("Application" 20 t) ("State" 15 t)
                               ("Active" 7 t) ("Tracked" 8 t) ("Wait" 24 t)]
        tabulated-list-padding 2)
  (add-hook 'tabulated-list-revert-hook #'emacsos-processes-refresh nil t)
  (tabulated-list-init-header))

(defun emacsos-processes-refresh ()
  "Refresh management rows through the public inspection API."
  (interactive)
  (setq tabulated-list-entries
        (mapcar (lambda (run)
                  (let ((id (plist-get run :id)))
                    (list id (vector (number-to-string id) (plist-get run :name)
                                     (symbol-name (plist-get run :state))
                                     (number-to-string (plist-get run :active))
                                     (number-to-string (length (plist-get run :resources)))
                                     (or (plist-get run :wait) "")))))
                (emacsos-app-list)))
  (tabulated-list-print t))

(defun emacsos-processes--selection ()
  (or (tabulated-list-get-id) (user-error "Select an application instance")))

(defun emacsos-processes-stop ()
  "Request cooperative stop for the selected instance."
  (interactive)
  (emacsos-app-request-stop (emacsos-processes--selection))
  (emacsos-processes-refresh))

(defun emacsos-processes-restart ()
  "Observe the selected terminal outcome and start a fresh invocation."
  (interactive)
  (let ((id (emacsos-processes--selection)))
    (emacsos-app-inspect id)
    (emacsos-app-restart id))
  (emacsos-processes-refresh))

(defun emacsos-processes-inspect ()
  "Show detached state, cleanup diagnostics and logs for the selected run."
  (interactive)
  (let ((snapshot (emacsos-app-inspect (emacsos-processes--selection))))
    (with-help-window "*Application details*"
      (princ "Trusted shared Emacs runtime: cooperative stop; no isolation or CPU/memory quotas.\n\n")
      (pp snapshot))))

(defun emacsos-processes-source ()
  "Visit the declared source of the selected run through the standard file API."
  (interactive)
  (let ((source (plist-get (emacsos-app-inspect (emacsos-processes--selection)) :source)))
    (unless source (user-error "No source file declared"))
    (find-file source)))

(defun emacsos-processes ()
  "Display a management client for trusted cooperative Elisp instances."
  (interactive)
  (pop-to-buffer (get-buffer-create "*Processes*"))
  (emacsos-processes-mode)
  (emacsos-processes-refresh))

(provide 'emacsos-app)
;;; emacsos-app.el ends here
