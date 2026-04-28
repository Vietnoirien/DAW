# ---- Stage 1: Build ----
# Nothing to compile for a static site — copy assets and configure nginx
FROM nginx:1.27-alpine AS runner

# Remove default nginx content
RUN rm -rf /usr/share/nginx/html/*

# Create non-root user
RUN addgroup --system --gid 1001 webuser \
 && adduser  --system --uid 1001 --ingroup webuser webuser

# Copy site source
COPY --chown=webuser:webuser src/ /usr/share/nginx/html/

# Copy screenshots
COPY --chown=webuser:webuser screenshots/ /usr/share/nginx/html/screenshots/

# Copy nginx configuration
COPY nginx.conf /etc/nginx/conf.d/default.conf

# nginx master process runs as root (required to bind port 80),
# but worker processes drop to nobody. This is the standard nginx pattern.
EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
